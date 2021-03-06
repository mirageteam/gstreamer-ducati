//#define LOG_NDEBUG 0

#undef LOG_TAG
#define LOG_TAG "GstMetadataRetrieverDriver"
//#define DEBUG_GST_WARNING

#include "GstMetadataRetrieverDriver.h"
#include <gst/video/video.h>
#include <fcntl.h>

#include <utils/Log.h>
#include <cutils/properties.h>

#define UNUSED(x) (void)x


using namespace android;

static GstStaticCaps static_audio_caps = GST_STATIC_CAPS ("audio/mpeg,framed=true;audio/mpeg,parsed=true;audio/AMR;audio/AMR-WB;audio/x-wma;audio/midi;audio/mobile-xmf");
static GstStaticCaps static_video_caps = GST_STATIC_CAPS ("video/mpeg;video/x-h263;video/x-h264;video/x-divx;video/x-wmv");

static GstStaticCaps static_have_video_caps = GST_STATIC_CAPS ("video/x-raw-yuv;video/x-raw-rgb");


GstMetadataRetrieverDriver::GstMetadataRetrieverDriver():
	mPipeline(NULL),
	mAppsrc(NULL),
	mUri(NULL),
	mTag_list(NULL),
	mFdSrcOffset_min(0), mFdSrcOffset_max(0), 
	mFdSrcOffset_current(0), mFd(-1),
	mState(GST_STATE_NULL),
	mAlbumArt(NULL)
{
	LOGV("constructor");

	mHaveStreamVideo = false;

	/* Add check wheather GLib threading system 
	 * has been initialised . Xuzhi.Tony
	 */
	if (!g_thread_get_initialized ()) {
	   g_thread_init(NULL);
	}

	init_gstreamer();
}

GstMetadataRetrieverDriver::~GstMetadataRetrieverDriver()
{
	LOGV("destructor");

	if(mTag_list) {
		LOGV("free tag list");
		gst_tag_list_free (mTag_list);
		mTag_list = NULL;
	}

	if(mAlbumArt) {
		gst_buffer_unref(mAlbumArt);
		mAlbumArt = NULL;
	}

	if(mPipeline) {	
		LOGV("free pipeline %s", gst_element_get_name(mPipeline));
		gst_element_set_state(mPipeline, GST_STATE_NULL);
		gst_object_unref (mPipeline);
		mPipeline = NULL;
	}

	if(mFd != -1) {
		close(mFd);
	}

	if(mUri) {
		g_free(mUri);
	}
}


void GstMetadataRetrieverDriver::setup(int mode)
{
	gchar  *description = NULL;
	GError *error = NULL;
	mMode = mode;

	if(mMode & METADATA_MODE_FRAME_CAPTURE_ONLY) {
		description = g_strdup_printf("uridecodebin uri=%s name=src ! ffmpegcolorspace ! videoscale ! appsink name=sink caps=\"video/x-raw-rgb,bpp=16\" enable-last-buffer=true", mUri);
	} else {
		description = g_strdup_printf("uridecodebin uri=%s name=src ! fakesink name=sink", mUri);
	}

	LOGV("create pipeline");
	mPipeline = gst_parse_launch(description, &error);

	if(!mPipeline)	{
		LOGE("can't create pipeline");
		return;
	}
	LOGV("pipeline creation: %s", gst_element_get_name(mPipeline));

	// verbose info (as gst-launch -v)
	// Activate the trace with the command: "setprop persist.gst.verbose 1"
	char value[PROPERTY_VALUE_MAX];
	property_get("persist.gst.verbose", value, "0");
	LOGV("persist.gst.verbose property = %s", value);
	if (value[0] == '1') {
		LOGV("Activate deep_notify");
		g_signal_connect (mPipeline, "deep_notify",
		        	  G_CALLBACK (gst_object_default_deep_notify), NULL);
	}
	
	mState = GST_STATE_NULL;
}


GstAutoplugSelectResult GstMetadataRetrieverDriver::autoplug_select_cb (
	GstElement *bin, GstPad *pad, GstCaps *caps, 
	GstElementFactory *factory, gpointer user_data)
{
	UNUSED(bin);
	UNUSED(caps);
	UNUSED(user_data);
	UNUSED(pad);
	const gchar* str;

    	str = gst_element_factory_get_longname (factory);
	
	/* xuzhi.Tony:
	 * Check if current codec is use hardware 
	 * skip it.
	 */
    	if(g_strrstr (str, "DMAI xDM 1.2"))
		return GST_AUTOPLUG_SELECT_SKIP;
    	else
		return GST_AUTOPLUG_SELECT_TRY;

}
void GstMetadataRetrieverDriver::setDataSource(const char* url)
{
	LOGV("create source from uri %s", url);

	if(!gst_uri_is_valid(url)) {
		gchar *uri_file = g_filename_to_uri (url, NULL, NULL);
		// add \" to avoid issues with space charactere in filename/filepath
		mUri = g_strdup_printf("\"%s\"", uri_file);
		g_free (uri_file);
	}
	else {
		mUri = g_strdup_printf("%s", url);
	}
	
	LOGV("set uri %s to src", mUri);
}

/*static*/ 
gboolean GstMetadataRetrieverDriver::have_video_caps (GstElement * uridecodebin, GstCaps * caps)
{
	GstCaps *intersection, *video_caps;
	
	gboolean res;
	
	video_caps = gst_static_caps_get(&static_have_video_caps);
	GST_OBJECT_LOCK(uridecodebin);
	intersection = gst_caps_intersect (caps, video_caps);
	GST_OBJECT_UNLOCK(uridecodebin);
	
	res = !(gst_caps_is_empty (intersection));

	gst_caps_unref (intersection);
	gst_caps_unref (video_caps);
	return res;
}

/*static*/ 
gboolean GstMetadataRetrieverDriver::are_audio_caps (GstElement * uridecodebin, GstCaps * caps)
{
	GstCaps *intersection, *end_caps;
	
	gboolean res;
	
	end_caps = gst_static_caps_get(&static_audio_caps);
	GST_OBJECT_LOCK(uridecodebin);
	intersection = gst_caps_intersect (caps, end_caps);
	GST_OBJECT_UNLOCK(uridecodebin);
	
	res = (gst_caps_is_empty (intersection));

	gst_caps_unref (intersection);
	gst_caps_unref (end_caps);
	return res;
}

/*static*/ 
gboolean GstMetadataRetrieverDriver::are_video_caps (GstElement * uridecodebin, GstCaps * caps)
{
	GstCaps *intersection, *end_caps;
	
	gboolean res;
	
	end_caps = gst_static_caps_get(&static_video_caps);
	GST_OBJECT_LOCK(uridecodebin);
	intersection = gst_caps_intersect (caps, end_caps);
	GST_OBJECT_UNLOCK(uridecodebin);
	
	res = (gst_caps_is_empty (intersection));

	gst_caps_unref (intersection);
	gst_caps_unref (end_caps);
	return res;
}


/* return TRUE if we continu to buld the graph, FALSE either */
/*static */ 
gboolean GstMetadataRetrieverDriver::autoplug_continue (GstElement* object,
							GstPad* pad,
                            GstCaps* caps,
                            GstMetadataRetrieverDriver* ed)
{
	GstStructure *structure = NULL;
	structure = gst_caps_get_structure (caps, 0);
	gboolean res;
	

	UNUSED(pad);
	
	//LOGV("autoplug_continue %s" ,gst_structure_get_name(structure));
	if(are_video_caps(object, caps)) {
		//LOGV("\nfound video caps %" GST_PTR_FORMAT, caps);
		ed->mHaveStreamVideo = TRUE;
	}
	
	res = are_audio_caps(object, caps);
	
	if(res && (ed->mMode & METADATA_MODE_METADATA_RETRIEVAL_ONLY)) {
		res &= are_video_caps(object, caps);
	}

	return res;
}

/*static*/ 
void GstMetadataRetrieverDriver::need_data (GstElement * object, guint size, GstMetadataRetrieverDriver* ed)
{
	GstFlowReturn ret;
	GstBuffer *buffer;
	UNUSED(object);

	if(ed->mFdSrcOffset_current >= ed->mFdSrcOffset_max) {
		LOGV("appsrc send eos");
		g_signal_emit_by_name (ed->mAppsrc, "end-of-stream", &ret);
		return;
	}	

	if((ed->mFdSrcOffset_current + size) > ed->mFdSrcOffset_max) {
		size = ed->mFdSrcOffset_max - ed->mFdSrcOffset_current;
	} 

	buffer = gst_buffer_new_and_alloc(size);

	if(buffer == NULL) {
		LOGV("appsrc can't allocate buffer of size %d", size);
		return;
	}
	size = read(ed->mFd, GST_BUFFER_DATA(buffer), GST_BUFFER_SIZE(buffer));
		
	GST_BUFFER_SIZE(buffer) = size;
	/* we need to set an offset for random access */
	GST_BUFFER_OFFSET (buffer) = ed->mFdSrcOffset_current - ed->mFdSrcOffset_min;
	ed->mFdSrcOffset_current += size;
	GST_BUFFER_OFFSET_END (buffer) = ed->mFdSrcOffset_current - ed->mFdSrcOffset_min;

	g_signal_emit_by_name (ed->mAppsrc, "push-buffer", buffer, &ret);
	gst_buffer_unref (buffer);
}

/*static*/ 
gboolean GstMetadataRetrieverDriver::seek_data(GstElement * object, guint64 offset, GstMetadataRetrieverDriver* ed)
{
	UNUSED(object);

	if((ed->mFdSrcOffset_min + offset) <= ed->mFdSrcOffset_max) {
		lseek (ed->mFd, ed->mFdSrcOffset_min + offset, SEEK_SET);
		ed->mFdSrcOffset_current = ed->mFdSrcOffset_min + offset;
	}

	return TRUE;
}

/*static*/ 
void GstMetadataRetrieverDriver::source_changed_cb (GObject *obj, GParamSpec *pspec, GstMetadataRetrieverDriver* ed)
{
	UNUSED(pspec);

	// get the newly created source element 
	g_object_get (obj, "source", &(ed->mAppsrc), (gchar*)NULL);

	if(ed->mAppsrc != NULL) {
		lseek (ed->mFd, ed->mFdSrcOffset_min, SEEK_SET);

		g_object_set(ed->mAppsrc, "format" , GST_FORMAT_BYTES, NULL);
		g_object_set(ed->mAppsrc, "stream-type" , 2 /*"random-access"*/ , NULL);
		g_object_set(ed->mAppsrc, "size", (gint64) (ed->mFdSrcOffset_max - ed->mFdSrcOffset_min), NULL);
		g_signal_connect (ed->mAppsrc, "need-data", G_CALLBACK (need_data), ed);
		g_signal_connect (ed->mAppsrc, "seek-data", G_CALLBACK (seek_data), ed);
	}
}


void GstMetadataRetrieverDriver::setFdDataSource(int fd, gint64 offset, gint64 length)
{
	LOGV("create source from fd %d offset %lld lenght %lld", fd, offset, length);

	// duplicate the fd because it should be close in java layers before we can use it
	mFd = dup(fd);
	LOGV("dup(fd) old %d new %d", fd, mFd);
	// create the uri string with the new fd
	mUri =  g_strdup_printf ("appsrc://"); 
	mFdSrcOffset_min = offset;
	mFdSrcOffset_current = mFdSrcOffset_min;
	mFdSrcOffset_max = mFdSrcOffset_min + length;	
}

void GstMetadataRetrieverDriver::getVideoSize(int* width, int* height)
{
	*width = 0;
   	*height = 0;

	if (mHaveStreamVideo) {
		GstElement * sink = gst_bin_get_by_name (GST_BIN(mPipeline), "sink");
		if (GstPad* pad = gst_element_get_static_pad(sink, "sink")) {
			gst_video_get_size(GST_PAD(pad), width, height);
			gst_object_unref(GST_OBJECT(pad));
		}
		LOGV("video width %d height %d", *width, *height);
	} 
}


void GstMetadataRetrieverDriver::getFrameRate(int* framerate)
{
	*framerate = 0;

	if (mHaveStreamVideo) {
		const GValue* fps = NULL;
		GstElement * sink = gst_bin_get_by_name (GST_BIN(mPipeline), "sink");
		if (GstPad* pad = gst_element_get_static_pad(sink, "sink")) {
			fps = gst_video_frame_rate(GST_PAD(pad));
			if (fps != NULL && GST_VALUE_HOLDS_FRACTION (fps)) {
				*framerate = gst_value_get_fraction_numerator (fps) / gst_value_get_fraction_denominator (fps);
			}
			gst_object_unref(GST_OBJECT(pad));
		}
		LOGV("framerate %d", *framerate);
	} 
}

#define PREPARE_SYNC_TIMEOUT 5000 * GST_MSECOND

void GstMetadataRetrieverDriver::prepareSync()
{
	GstBus * bus = NULL;
	GstMessage *message = NULL;
	GstElement * src = NULL;
	GstMessageType message_filter = (GstMessageType)(GST_MESSAGE_ERROR | GST_MESSAGE_ASYNC_DONE);

	if(mMode & METADATA_MODE_METADATA_RETRIEVAL_ONLY) {
		message_filter = (GstMessageType)(GST_MESSAGE_ERROR | GST_MESSAGE_ASYNC_DONE | GST_MESSAGE_TAG);
	}

	LOGV("prepareSync");
	src = gst_bin_get_by_name (GST_BIN(mPipeline), "src");

	if(src == NULL) {
		LOGV("prepareSync no src found");
		mState = GST_STATE_NULL;
		return;
	}

	g_signal_connect (src, "autoplug-continue", G_CALLBACK (autoplug_continue),this);

        /* connect this signal to skip hardware decoder.
         * xuzhi.Tony
         */
	g_signal_connect (src, "autoplug-select", G_CALLBACK (autoplug_select_cb), this);

	if(mFdSrcOffset_max) {
		g_signal_connect (src, "notify::source", G_CALLBACK (source_changed_cb),this);
	}

	bus = gst_pipeline_get_bus (GST_PIPELINE(mPipeline));
	gst_element_set_state(mPipeline, GST_STATE_READY);
	
	gst_element_set_state(mPipeline, GST_STATE_PAUSED);

	message = gst_bus_timed_pop_filtered(bus, PREPARE_SYNC_TIMEOUT, message_filter);
	
//	mState = GST_STATE_PAUSED;

	while(message != NULL) {
		switch(GST_MESSAGE_TYPE(message)) {
			case GST_MESSAGE_TAG: 
			{
				GstTagList *tag_list, *result;

				LOGV("receive TAGS from the stream");
				gst_message_parse_tag (message, &tag_list);

				/* all tags (replace previous tags, title/artist/etc. might change
				* in the middle of a stream, e.g. with radio streams) */
				result = gst_tag_list_merge (mTag_list, tag_list, GST_TAG_MERGE_REPLACE);
				if (mTag_list)
					gst_tag_list_free (mTag_list);
				mTag_list = result;

				/* clean up */
				gst_tag_list_free (tag_list);
				gst_message_unref(message);
				break;
			}

			case GST_MESSAGE_ASYNC_DONE:
			{
				mState = GST_STATE_PAUSED;
				LOGV("receive GST_MESSAGE_ASYNC_DONE");
				gst_message_unref(message);
				goto bail;
			}

			case GST_MESSAGE_ERROR:
			{
				GError* err;
				gchar* debug;

				mState = GST_STATE_NULL;
				gst_message_parse_error(message, &err, &debug);
				LOGV("receive GST_MESSAGE_ERROR : %d, %s (EXTRA INFO=%s)",
					err->code,
					err->message,
					(debug != NULL) ? debug : "none");
				gst_message_unref(message);
                		if (debug) {
					g_free (debug);
                		}
				goto bail;
			}

			default:
				// do nothing
				break;
		}
		message = gst_bus_timed_pop_filtered(bus, 50*GST_MSECOND, message_filter);
	}

bail:
	gst_object_unref(bus);
}


void GstMetadataRetrieverDriver::seekSync(gint64 p)
{
	gint64 position;
	GstMessage *message = NULL;
	GstBus * bus = NULL;

	if(!mPipeline){
		LOGE("mPipeline is NULL ,return");
		return;
	}

	position = ((gint64)p) * 1000000;
	LOGV("Seek to %lld ms (%lld ns)", p, position);
	if(!gst_element_seek_simple(mPipeline, GST_FORMAT_TIME, (GstSeekFlags)((int)GST_SEEK_FLAG_FLUSH | (int)GST_SEEK_FLAG_KEY_UNIT), position)) {
		LOGE("Can't perfom seek for %lld ms", p);
	}
	
	bus = gst_pipeline_get_bus (GST_PIPELINE(mPipeline));

	message = gst_bus_timed_pop_filtered(bus, PREPARE_SYNC_TIMEOUT, (GstMessageType)(GST_MESSAGE_ERROR | GST_MESSAGE_ASYNC_DONE));
	
	if( message != NULL) {
		switch(GST_MESSAGE_TYPE(message)) {

			case GST_MESSAGE_ASYNC_DONE:
			{
				mState = GST_STATE_PAUSED;
				break;
			}

			case GST_MESSAGE_ERROR:
			{
				mState = GST_STATE_NULL;
				break;
			}
			default:
				// do nothing
				break;
		}
		gst_message_unref(message);
	}
	gst_object_unref(bus);
}


gint64 GstMetadataRetrieverDriver::getPosition()
{
	GstFormat fmt = GST_FORMAT_TIME;
	gint64 pos = 0;

	if(!mPipeline) {
		LOGV("get postion but pipeline has not been created yet");
		return 0;
	}	

	LOGV("getPosition");
	gst_element_query_position (mPipeline, &fmt, &pos);
	LOGV("Stream position %lld ms", pos / 1000000);
	return (pos / 1000000);	
}

int GstMetadataRetrieverDriver::getStatus()
{
	return mState;
}

gint64 GstMetadataRetrieverDriver::getDuration()
{

	GstFormat fmt = GST_FORMAT_TIME;
	gint64 len;

	if(!mPipeline) {
		LOGE("get duration but pipeline has not been created yet");
		return 0;
	}	

	// the duration given by gstreamer is in nanosecond 
	// so we need to transform it in millisecond
	LOGV("getDuration");
	if(gst_element_query_duration(mPipeline, &fmt, &len)) {
		LOGV("Stream duration %lld ms", len / 1000000 );
	} 
	else {
		LOGV("Query duration failed");
		len = 0;
	}

	if ((GstClockTime)len == GST_CLOCK_TIME_NONE) {
		LOGV("Query duration return GST_CLOCK_TIME_NONE");
		len = 0;
	}

	return (len / 1000000);	
}

void GstMetadataRetrieverDriver::quit()
{	
	int state = -1;

	LOGV("quit");


	if(mTag_list) {
		LOGV("free tag list");
		gst_tag_list_free (mTag_list);
		mTag_list = NULL;
	}

	if(mPipeline) {
		GstBus *bus;
		bus = gst_pipeline_get_bus(GST_PIPELINE (mPipeline));
		LOGV("flush bus messages");
		if (bus != NULL) {
			gst_bus_set_flushing(bus, TRUE);
			gst_object_unref (bus);
		}
		LOGV("free pipeline %s", gst_element_get_name(mPipeline));
		state = gst_element_set_state(mPipeline, GST_STATE_NULL);
		LOGV("set pipeline state to NULL: %d (0:Failure, 1:Success, 2:Async, 3:NO_PREROLL)", state);
		gst_object_unref (mPipeline);

		mPipeline = NULL;
		
	}

	mState = GST_STATE_NULL;
}

void GstMetadataRetrieverDriver::getCaptureFrame(guint8** data)
{
	LOGV("getCaptureFrame");

	if(mPipeline != NULL) {
		GstBuffer* frame = NULL;
		GstElement * sink = gst_bin_get_by_name (GST_BIN(mPipeline), "sink");

		g_object_get(G_OBJECT(sink), "last-buffer" , &frame, NULL);

		if(frame != NULL) {
			if(*data) {
				delete [] *data;
			}
			*data = new guint8[GST_BUFFER_SIZE(frame)];
			memcpy(*data, GST_BUFFER_DATA(frame), GST_BUFFER_SIZE(frame));
			/* change gst_object_unref to gst_buffer_unref to release frame----changed by gale */
			gst_buffer_unref(frame);
		}
	}
}


gchar* GstMetadataRetrieverDriver::getMetadata(gchar *tag)
{
	LOGV("get metadata tag %s", tag);

	gchar *str;
	gint count;

	if(!mTag_list) // no tag list nothing do to
	{
		LOGV("No taglist => Nothing to do");
		return NULL;
	}
	
	count = gst_tag_list_get_tag_size (mTag_list, tag);
	if(count) {

		if (gst_tag_get_type (tag) == G_TYPE_STRING) {
			if (!gst_tag_list_get_string_index (mTag_list, tag, 0, &str)) {
				g_assert_not_reached ();
			}
		} else {
			str = g_strdup_value_contents (gst_tag_list_get_value_index (mTag_list, tag, 0));
		}
		LOGV("for tag %s have metadata %s",  tag, str);	
		return str;
	}
	else {
		LOGV(" No Tag : %s ! ",tag);
	}
	return NULL;
}

void GstMetadataRetrieverDriver::getAlbumArt(guint8 **data, guint64 *size)
	{
	if(mAlbumArt == NULL)  {
		LOGV("getAlbumArt try to get image from tags");
		if(mTag_list)
		{
			gboolean res = FALSE;
			res = gst_tag_list_get_buffer (mTag_list, GST_TAG_IMAGE, &mAlbumArt);
			if(!res)
				res = gst_tag_list_get_buffer (mTag_list, GST_TAG_PREVIEW_IMAGE, &mAlbumArt);
	
			if(!res)
				LOGV("no album art found");
		}
	}

	if(mAlbumArt) {
		*data = GST_BUFFER_DATA(mAlbumArt);
		*size= GST_BUFFER_SIZE(mAlbumArt);
	}
}

/*static*/ 
void GstMetadataRetrieverDriver::debug_log (GstDebugCategory * category, GstDebugLevel level,
							const gchar * file, const gchar * function, gint line,
							GObject * object, GstDebugMessage * message, gpointer data)
{
	gint pid;
	GstClockTime elapsed;
	GstMetadataRetrieverDriver* ed = (GstMetadataRetrieverDriver*)data;

	UNUSED(file);
	UNUSED(object);

	if (level > gst_debug_category_get_threshold (category))
	    return;

	pid = getpid ();

	elapsed = GST_CLOCK_DIFF (ed->mGst_info_start_time,
			gst_util_get_timestamp ());


	g_printerr ("%" GST_TIME_FORMAT " %5d %s %s %s:%d %s\r\n",
		GST_TIME_ARGS (elapsed),
		pid,
		gst_debug_level_get_name (level),
		gst_debug_category_get_name (category), function, line,
		gst_debug_message_get (message));
}


void GstMetadataRetrieverDriver::init_gstreamer() 
{
	// do the init of gstreamer there
	GError *err = NULL;

	int argc=3;
	char **argv;
	char str0[] =  "";
	char str2[] =  "";
	char debug[PROPERTY_VALUE_MAX];
	char trace[PROPERTY_VALUE_MAX];


	argv = (char **)malloc(sizeof(char *) * argc);          
	argv[0] = (char *) malloc( sizeof(char) * (strlen(str0) + 1));        
	strcpy( argv[0], str0);     
	argv[2] = (char *) malloc( sizeof(char) * (strlen(str2) + 1));        
	strcpy( argv[2], str2);     

	property_get("persist.gst.debug", debug, "0");
	LOGV("persist.gst.debug property %s", debug);
	argv[1] = (char *) malloc( sizeof(char) * (strlen(debug) + 1));         
	strcpy( argv[1], debug);  
	
	property_get("persist.gst.trace", trace, "/dev/console");
	LOGV("persist.gst.trace property %s", trace);
	LOGV("route the trace to %s", trace);
	int fd_trace = open(trace, O_RDWR);
	if(fd_trace != -1) {
		dup2(fd_trace, 0);
		dup2(fd_trace, 1);
		dup2(fd_trace, 2);
		close(fd_trace);
	}
	
	mGst_info_start_time = gst_util_get_timestamp ();
#ifdef DEBUG_GST_WARNING //add by xuzhi.Tony
        gst_debug_set_default_threshold(GST_LEVEL_WARNING);
        gst_debug_add_log_function (gst_debug_log_default,NULL);
#else
        gst_debug_remove_log_function (debug_log);
        gst_debug_add_log_function (debug_log, this);
        gst_debug_remove_log_function (gst_debug_log_default);
#endif
	LOGV("gstreamer init check");
	// init gstreamer 	
	if(!gst_init_check (&argc, &argv, &err))
	{
		LOGE ("Could not initialize GStreamer: %s\n", err ? err->message : "unknown error occurred");
		if (err) {
			g_error_free (err);
		}
	}

}
