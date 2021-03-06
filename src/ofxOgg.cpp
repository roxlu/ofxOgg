#include "ofxOgg.h"

#include "Poco/Glob.h"
#include "Poco/Path.h"

ofxOgg::ofxOgg() 
:is_enabled(true)
{
	ofAddListener(ofEvents.setup, this, &ofxOgg::onSetup);
}

ofxOgg::~ofxOgg() {
	ogg_page oggpage;
	while (ogg_stream_flush(&oggss, &oggpage) > 0) {
		fwrite(oggpage.header, oggpage.header_len, 1, out_file);
		fwrite(oggpage.body  , oggpage.body_len  , 1, out_file);
	}
	
	
	th_encode_free(context);
	ogg_stream_clear(&oggss);
	free(yuv_y);
	free(yuv_u);
	free(yuv_v);
	fclose(out_file);
}

void ofxOgg::onSetup(ofEventArgs& ev) {
	setup(ofGetWidth(), ofGetHeight(), 3);
}

void ofxOgg::setup(int w, int h, int bytesPerPixel) {
	string of_dp = ofToDataPath("./", true);
	string dp = of_dp +"*.ogv";
	std::set<std::string> files;
	Poco::Glob::glob(dp, files);
	std:set<std::string>::iterator it = files.begin();
	number_of_ogg_files = files.size();
	grab_image.allocate(w,h,OF_IMAGE_COLOR);
	
	char buf[512];
	sprintf(buf, "openFramweworksAppMovie-%04d.ogv", number_of_ogg_files);
	string filename(buf);
	output_file_path = ofToDataPath(filename, true);
	int i = 44;
	
	width = w;
	height = h;
	bpp = bytesPerPixel;
	
	// create info object with encoding info.
	// ----------------------------------------
	th_info_init(&info);    
	
    info.frame_width = ((w + 15) >>4)<<4; // why (?)
    info.frame_height = ((h + 15)>>4)<<4; // why (?)
    info.pic_width = w;
    info.pic_height = h;
    info.pic_x = 0;
    info.pic_y = 0;
	info.colorspace = TH_CS_UNSPECIFIED;
    info.pixel_fmt = TH_PF_420;
    info.fps_numerator = 25;
    info.fps_denominator = 1;
    info.aspect_numerator = 1;
    info.aspect_denominator = 1;
	info.quality = 63;
	// info.target_bitrate = 1018000;
    // info.keyframe_granule_shift = 0;
	
	// context to work with.
	// -----------------------
	context = th_encode_alloc(&info);  
	if(!context) {
		printf("Error: cannot create context.\n");
		exit(1);
	}
	else {
		printf("OK: context\n");
	}
	
    th_info_clear(&info);
	
	// Add obligatory headers
	// ---------------------
	th_comment comment;
	th_comment_init(&comment);
	th_comment_add(&comment, (char *)"openFrameworks");
	comment.vendor = (char *)"openFrameworks";
	
	out_file= fopen(output_file_path.c_str(), "w");
	if(!out_file) {
		printf("Error: cannot create output file.\n");
		exit(1);
	}
	
	ogg_packet oggpacket;
	ogg_page oggpage;
	ogg_stream_init(&oggss, rand());
	while (th_encode_flushheader(context, &comment, &oggpacket) > 0) {
		ogg_stream_packetin(&oggss, &oggpacket);
		while (ogg_stream_pageout(&oggss, &oggpage)) {
			fwrite(oggpage.header, oggpage.header_len, 1, out_file);
			fwrite(oggpage.body  , oggpage.body_len  , 1, out_file);
		}
	}
	 
	// rest of headers... before creating a new page.
	while (ogg_stream_flush(&oggss, &oggpage) > 0) {
		fwrite(oggpage.header, oggpage.header_len, 1, out_file);
		fwrite(oggpage.body  , oggpage.body_len  , 1, out_file);
	}
	
	// setup buffers to write to
	// --------------------------
	yuv_w = w;
	yuv_h = h;

	yuv_w = (w + 15) & ~15;
	yuv_h = (h + 15) & ~15;
  
	ycbcr[0].width = yuv_w;
	ycbcr[0].height = yuv_h;
	ycbcr[0].stride = yuv_w;
	ycbcr[1].width = (yuv_w >> 1);
	ycbcr[1].stride = ycbcr[1].width;
	ycbcr[1].height = (yuv_h >> 1);
	ycbcr[2].width = ycbcr[1].width;
	ycbcr[2].stride = ycbcr[1].stride;
	ycbcr[2].height = ycbcr[1].height;
	
	
	// @todo free buffers
	ycbcr[0].data = yuv_y = (unsigned char*)malloc(ycbcr[0].stride * ycbcr[0].height);
	ycbcr[1].data = yuv_u = (unsigned char*)malloc(ycbcr[1].stride * ycbcr[1].height);
	ycbcr[2].data = yuv_v = (unsigned char*)malloc(ycbcr[2].stride * ycbcr[2].height);
	
	
	// context used to convert RGB->YUV
	convert_context = sws_getContext(
		w
		,h
		,PIX_FMT_RGB24
		,w
		,h
		,PIX_FMT_YUV420P
		,SWS_FAST_BILINEAR
		,NULL
		,NULL
		,NULL
	);

	if(convert_context == NULL) {
		printf("Error: cannot get convert context\n");
		exit(1);
	}

	in_image = vpx_img_alloc(NULL, VPX_IMG_FMT_RGB24, width, height, 0);
	out_planes[0] = ycbcr[0].data;
	out_planes[1] = ycbcr[1].data;
	out_planes[2] = ycbcr[2].data;

	out_strides[0] = ycbcr[0].stride;
	out_strides[1] = ycbcr[1].stride;
	out_strides[2] = ycbcr[2].stride;

	line_size = w * 3;	
}

void ofxOgg::addFrame() {
	if(!is_enabled) {
		return;
	}
	
	grab_image.grabScreen(0,0,ofGetWidth(), ofGetHeight());
	unsigned char* pixels = grab_image.getPixels();
	
	// convert RGB to YUV
	// -------------------------------------------------------------------------
	in_image = vpx_img_wrap(in_image, VPX_IMG_FMT_RGB24, width, height, 0, pixels);
	int output_slice_h = sws_scale(
		 convert_context
		,in_image->planes
		,in_image->stride
		,0
		,height
		,out_planes
		,out_strides
	);	
	
	
	int enc_result = th_encode_ycbcr_in(context, ycbcr);
	if(enc_result == TH_EFAULT) {
		fprintf(stderr, "error: could not encode frame\n");
		exit(1);
	}
	
	ogg_page oggpage;
	ogg_packet oggpacket;
	int last = 0;
	static int frame_num = 0;
	frame_num++;
	while(th_encode_packetout(context, last, &oggpacket) > 0) {
		ogg_stream_packetin(&oggss, &oggpacket);
		while(ogg_stream_pageout(&oggss, &oggpage)) {
			fwrite(oggpage.header, oggpage.header_len, 1, out_file);
			fwrite(oggpage.body, oggpage.body_len, 1, out_file);
			printf("adding frame # %d\n", frame_num);
		}
	}		

}

string ofxOgg::getOutputFilePath() {
	return output_file_path;
}

void ofxOgg::enableRecording() {
	is_enabled = true;
}

void ofxOgg::disableRecording() {
	is_enabled = false;
}

bool ofxOgg::isRecordingEnabled() {
	return is_enabled;
}