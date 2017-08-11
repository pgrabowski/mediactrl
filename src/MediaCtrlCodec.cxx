/***************************************************************************
 *   Copyright (C) 2007 by Lorenzo Miniero (lorenzo.miniero@unina.it)      *
 *   University of Naples Federico II                                      *
 *   COMICS Research Group (http://www.comics.unina.it)                    *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

/*! \file
 *
 * \brief Media Codec/Frame Handler for Control Packages
 *
 * \author Lorenzo Miniero <lorenzo.miniero@unina.it>
 */

#include "MediaCtrlCodec.h"
#include <map>

extern "C" {
#ifdef FFMPEG_ALTDIR
#include <libavcodec/avcodec.h>	// FFmpeg libavcodec
#else
#include <ffmpeg/avcodec.h>	// FFmpeg libavcodec
#endif
}

using namespace mediactrl;
using namespace ost;

// Collector related stuff
class MediaCtrlFrameCollector : public Thread {
	public:
		MediaCtrlFrameCollector();
		~MediaCtrlFrameCollector();

		void addFrame(MediaCtrlFrame *frame);

	private:
		void run();

		bool active;
		list<MediaCtrlFrame*> frames;
		ost::Mutex mFrames;
};

static MediaCtrlFrameCollector *collector = NULL;

void *getCollector()
{
	return collector;
}

void setCommonCollector(void *frameCollector)
{
	collector = (MediaCtrlFrameCollector*)frameCollector;
}

void startCollector()
{
	if(collector == NULL) {
		collector = new MediaCtrlFrameCollector();
		collector->start();
	}
}

void stopCollector()
{
	if(collector) {
		delete collector;
		collector = NULL;
	}
}

MediaCtrlFrameCollector::MediaCtrlFrameCollector()
{
	cout << "[FRAME] Starting Collector" << endl;
	active = false;
}

MediaCtrlFrameCollector::~MediaCtrlFrameCollector()
{
	cout << "[FRAME] Destroying Collector" << endl;
	active = false;
	struct timeval now;
	gettimeofday(&now, NULL);
	time_t current = now.tv_sec*1000000 + now.tv_usec;
	mFrames.enter();
	if(!frames.empty()) {
		MediaCtrlFrame *frame = NULL;
		// Delete all frames
		while(!frames.empty()) {
			frame = frames.front();
			frames.pop_front();
			if(frame) {
#if 0
				cout << "[FRAME] \tDeleting frame " << frame << " (" << dec << frame->getTimeBorn() << "/" << dec << current << ")" << endl;
#endif
				delete frame;
				frame = NULL;
			}
		}
	}
	mFrames.leave();
}

void MediaCtrlFrameCollector::addFrame(MediaCtrlFrame *frame)
{
	if(!active)
		return;
	mFrames.enter();
	frames.push_back(frame);
	mFrames.leave();
}

void MediaCtrlFrameCollector::run()
{
	active = true;
	struct timeval now;
	time_t current;
	gettimeofday(&now, NULL);
	current = now.tv_sec*1000000 + now.tv_usec;
	MediaCtrlFrame *frame = NULL;
	list<MediaCtrlFrame*> oldFrames;
	oldFrames.clear();
	list<MediaCtrlFrame*>::iterator iter;
	unsigned int oldSize = 1;
	int rtp = 0, ivr = 0, mixer = 0, codec = 0, unknown = 0;	// debug
	while(active) {
		now.tv_sec = 1;
		now.tv_usec = 0;
		select(0, NULL, NULL, NULL, &now);
		gettimeofday(&now, NULL);
		current = now.tv_sec*1000000 + now.tv_usec;
		if(frames.empty())
			continue;
		mFrames.enter();
#if 0
		if(frames.empty() && (oldSize == 0))
			continue;
		mFrames.enter();
		cout << "[FRAME] Collector's handling " << dec << frames.size() << " frames" << endl;
		oldSize = frames.size();
		rtp = 0;
		ivr = 0;
		mixer = 0;
		codec = 0;
		unknown = 0;
		iter = frames.begin();
		for(iter = frames.begin(); iter != frames.end(); iter++) {
			if(!(*iter))
				continue;
			switch((*iter)->getAllocator()) {
				case RTP:
					rtp++;
					break;
				case IVR:
					ivr++;
					break;
				case MIXER:
					mixer++;
					break;
				case CODEC:
					codec++;
					break;
				default:
					unknown++;
					break;
			}
		}
		cout << "[FRAME] \tRTP:   " << dec << rtp << endl;
		cout << "[FRAME] \tIVR:   " << dec << ivr << endl;
		cout << "[FRAME] \tMIXER: " << dec << mixer << endl;
		cout << "[FRAME] \tCODEC: " << dec << codec << endl;
		cout << "[FRAME] \t?????: " << dec << unknown << endl;
#endif
		// For each frame, check how many time has passed, and in case it's old free the frame
		iter = frames.begin();
		for(iter = frames.begin(); iter != frames.end(); iter++) {
			frame = (*iter);
			if(!frame)
				continue;
			if((current-frame->getTimeBorn()) >= 3000000)
				oldFrames.push_back(frame);
		}
		if(!oldFrames.empty()) {
			while(!oldFrames.empty()) {
				frame = oldFrames.front();
				oldFrames.pop_front();
				frames.remove(frame);
				delete frame;
				frame = NULL;
			}
		}
		mFrames.leave();
	}
}


// MediaCtrlFrame
MediaCtrlFrame::MediaCtrlFrame(int media)
{
	this->media = media;
	ts = 0;
	if(media == MEDIACTRL_MEDIA_AUDIO)
		ts = 160;
	format = MEDIACTRL_RAW;
	buffer = NULL;
	len = 0;
	setNormal();
	counter = 0;
	frames = NULL;
	allocated = false;
	original = NULL;
	owner = NULL;
	who = 0;
	tid = "";
	// Mark when the frame has been added
	struct timeval now;
	gettimeofday(&now, NULL);
	timeBorn = now.tv_sec*1000000 + now.tv_usec;
	// Add to the collector
	if(collector)
		collector->addFrame(this);
}

MediaCtrlFrame::MediaCtrlFrame(int media, uint8_t *buffer, int len, int format)
{
	allocated = false;
	this->media = media;
	ts = 0;
	if(media == MEDIACTRL_MEDIA_AUDIO) {
		flags = MEDIACTRL_FLAG_NONE;
		ts = 160;
	}
	setFormat(format);
	setBuffer(buffer, len);
	setNormal();
	counter = 0;
	frames = NULL;
	original = NULL;
	owner = NULL;
	who = 0;
	tid = "";
	// Mark when the frame has been added
	struct timeval now;
	gettimeofday(&now, NULL);
	timeBorn = now.tv_sec*1000000 + now.tv_usec;
	// Add to the collector
	if(collector)
		collector->addFrame(this);
}

MediaCtrlFrame::~MediaCtrlFrame()
{
	if(buffer) {
		if(media == MEDIACTRL_MEDIA_AUDIO) {
			if(buffer)
				MCMFREE(buffer);
			buffer = NULL;
		}
		if(frames != NULL) {	// There might be appended frames, free them too
//			while(!frames->empty()) {
//				MediaCtrlFrame *frame = frames->front();
//				frames->pop_front();
//				if(frame != NULL)
//					delete frame;
//			}
			frames->clear();
			delete frames;
			frames = NULL;
		}
	}
}

void MediaCtrlFrame::setBuffer(uint8_t *buffer, int len)
{
	this->len = len;
	if(media == MEDIACTRL_MEDIA_AUDIO) {
		allocated = true;
		this->buffer = (uint8_t*)MCMALLOC(len, sizeof(uint8_t));
		memcpy(this->buffer, buffer, len);
	}
}

void MediaCtrlFrame::appendFrame(MediaCtrlFrame *frame)
{
	if(frames == NULL)
		frames = new MediaCtrlFrames;
	frames->push_back(frame);
}
