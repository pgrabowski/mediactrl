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
 * \brief Real-time Transport Protocol (RTP) Transactions Handler
 *
 * \author Lorenzo Miniero <lorenzo.miniero@unina.it>
 *
 * \ingroup core
 * \ref core
 */

#include "MediaCtrlRtp.h"

using namespace mediactrl;

string random_string(size_t size);
string random_string(size_t size)
{
	long val[4];
	int x;

	for (x = 0; x < 4; x++)
		val[x] = random();
	char buf[size];
	memset(buf, 0, size);
	snprintf(buf, size, "%08lx%08lx%08lx%08lx", val[0], val[1], val[2], val[3]);

	string label = buf;
	return label;
}


/// A static indicator that checks if the oRTP library has already been initialized
static bool ortp_initialized=false;

/// A static PayloadType instance for H.264, since oRTP doesn't have one
PayloadType payload_type_h264;

void rtpSetup()
{
	ortp_initialized = true;
	ortp_init();
	ortp_scheduler_init();
	ortp_set_log_level_mask(ORTP_DEBUG|ORTP_MESSAGE|ORTP_WARNING|ORTP_ERROR);

	// Codec and payload type profiling (FIXME)
	rtp_profile_set_payload(&av_profile, 101, &payload_type_telephone_event);
}

void rtpCleanup()
{
	ortp_exit();
}


// oRTP callbacks
void mediactrl_rtp_ssrc_changed(RtpSession *session, unsigned long data)
{
	MediaCtrlRtpChannel *rtpChannel = (MediaCtrlRtpChannel *)data;
	if(!rtpChannel)
		return;

	cout << "[RTP] SSRC changed" << endl;
}

void mediactrl_rtp_pt_changed(RtpSession *session, int pt, unsigned long data)
{
	if(pt == 101) {
		cout << "[RTP] Payload Type reports a telephone event" << endl;
		return;
	}

	cout << "[RTP] Payload type changed --> " << dec << (uint16_t)pt << endl;
	if(pt > 127) {
		cout << "[RTP] \t\t" << dec << pt << " >> 127, WTF??" << endl;
		return;
	}

	MediaCtrlRtpChannel *rtpChannel = (MediaCtrlRtpChannel *)data;
	if(!rtpChannel)
		return;

	cout << "[RTP] Notifying payload change" << endl;
	pt = rtp_session_get_recv_payload_type(session);
	rtpChannel->setPayloadType(pt);
}

void mediactrl_rtp_event_pkg(RtpSession *session, int type, unsigned long data)
{
	MediaCtrlRtpChannel *rtpChannel = (MediaCtrlRtpChannel *)data;
	if(!rtpChannel)
		return;

	cout << "[RTP] Telephony event package?" << endl;
}

void mediactrl_rtp_event(RtpSession *session, int type, unsigned long data)
{
	MediaCtrlRtpChannel *rtpChannel = (MediaCtrlRtpChannel *)data;
	if(!rtpChannel)
		return;
	cout << "[RTP] Telephony event: " << dec << type << endl;

	rtpChannel->incomingDtmf(type);
}

void mediactrl_rtp_error(RtpSession *session, unsigned long data)
{
	MediaCtrlRtpChannel *rtpChannel = (MediaCtrlRtpChannel *)data;
	if(!rtpChannel)
		return;
// FIXME
}

void mediactrl_rtp_ts_jump(RtpSession *session, unsigned long data)
{
	MediaCtrlRtpChannel *rtpChannel = (MediaCtrlRtpChannel *)data;
	if(!rtpChannel)
		return;

//	cout << "[RTP] Timestamp jump" << endl;
	// TODO Update current receival timestamp
	rtp_session_resync(session);
}


// The RTP Class
MediaCtrlRtpChannel::MediaCtrlRtpChannel(const InetHostAddress &ia, int media)
{
	this->media = media;	// FIXME involve media type in RTP profiling
	if(media == MEDIACTRL_MEDIA_AUDIO) {
		clockrate = 160;	// FIXME This is for 8000 audio codecs
		timing = 20000;		// FIXME 8Khz = 50 samples per second = 20ms
		flags = 0;
	}

	// This only needs to be done once
	if(!ortp_initialized)
		rtpSetup();

	rtpSession = rtp_session_new(RTP_SESSION_SENDRECV);
	rtp_session_set_local_addr(rtpSession, "0.0.0.0", -1);	// Choose a random port
	rtp_session_set_scheduling_mode(rtpSession, TRUE);	// FIXME
	rtp_session_set_blocking_mode(rtpSession, TRUE);	// FIXME
	rtp_session_set_connected_mode(rtpSession, TRUE);
	rtp_session_set_symmetric_rtp(rtpSession, TRUE);
	rtp_session_set_profile(rtpSession, &av_profile);
	rtp_session_set_payload_type(rtpSession, 0);				// FIXME
	rtp_session_set_source_description(rtpSession,
					"mediactrl@localhost",			// cname
					"University of Napoli Federico II",	// name
					"lorenzo.miniero@unina.it",		// email
					NULL,					// phone
					NULL,					// loc
					"mediactrl-prototype-0.2.0",		// tool
					"This is free software (GPL) !");	// note
	if(media == MEDIACTRL_MEDIA_AUDIO) {
		rtp_session_enable_adaptive_jitter_compensation(rtpSession, TRUE);	// FIXME
		rtp_session_set_jitter_compensation(rtpSession, 40);		// FIXME
	}
	rtp_session_signal_connect(rtpSession, "ssrc_changed", (RtpCallback)mediactrl_rtp_ssrc_changed, (unsigned long)this);
	rtp_session_signal_connect(rtpSession, "payload_type_changed", (RtpCallback)mediactrl_rtp_pt_changed, (unsigned long)this);
	rtp_session_signal_connect(rtpSession, "telephone-event", (RtpCallback)mediactrl_rtp_event, (unsigned long)this);
	rtp_session_signal_connect(rtpSession, "network_error", (RtpCallback)mediactrl_rtp_error, (unsigned long)this);
	rtp_session_signal_connect(rtpSession, "timestamp_jump", (RtpCallback)mediactrl_rtp_ts_jump, (unsigned long)this);

	// Make use of the opaque pointer to reference ourselves
	rtpSession->user_data = this;

	srcPort = rtp_session_get_local_port(rtpSession);
	cout << "[RTP] Creating new RTP connection (local port will be " << srcPort << ")" << endl;
	srcIp = ia;
	dstPort = 0;
	pt = 0;
	direction = MEDIACTRL_SENDRECV;
	locked = false;
	lockOwner = NULL;
	num = 0;
	lastIncomingTs = 0;

	label = random_string(8);
	cout << "[RTP] Label for this new RTP connection is " << label << endl;

	codec = NULL;

	tones.clear();
	mTones = new ost::Mutex();

	packets.clear();
	packetLens.clear();

	alive = false;

	active = false;
	cond = new ost::Conditional();
}

MediaCtrlRtpChannel::~MediaCtrlRtpChannel()
{
	cout << "[RTP] Destroying RTP connection bound to port " << srcPort << endl;
	if(alive) {
		wakeUp(true);
		alive = false;
		join();
	}
	// First of all, notify who cares...
	if(rtpManager != NULL)
		rtpManager->channelClosed(label);
	// ... and then free everything
	rtp_session_destroy(rtpSession);
	if(codec != NULL)
		delete codec;
	delete mTones;
	delete cond;
}

bool MediaCtrlRtpChannel::setPeer(const InetHostAddress &ia, uint16_t dataPort)
{
	cout << "[RTP] Setting peer for /" << srcPort << " --> " << ia << ":" << dataPort << "..." << endl;
	if((ia == dstIp) && (dataPort == dstPort))
		return true;

	bool startup = false;
	if(dstPort == 0)
		startup = true;

	dstIp = ia;
	dstPort = dataPort;
	rtp_session_set_remote_addr(rtpSession, ia.getHostname(), dataPort);
	cout << "[RTP]     Peer set to " << dstIp << ":" << dstPort << " (" << label << ")" << endl;

	if(startup)
		start();

	return true;
}

void MediaCtrlRtpChannel::setPayloadType(int pt)
{
	// TODO Change codec, if needed
	this->pt = pt;
	rtp_session_set_payload_type(rtpSession, pt);

	if((rtpManager != NULL) && (media == MEDIACTRL_MEDIA_AUDIO)) {
		// Open related codec, destroying the old one if necessary
		if(codec == NULL) {
			codec = rtpManager->createCodec(pt);
			if(codec == NULL)
				return;
		}
		if(!codec->hasStarted()) {
			if(codec->start() == false) {
				cout << "[RTP] Codec failed to startup... (" << label << ")" << endl;
				delete codec;
				codec = NULL;
			}
			if(codec != NULL) {
				cout << "[RTP] Codec started (" << label << ")" << endl;
				if(codec->getCodecId() == MEDIACTRL_CODEC_H264) {
					flags = MEDIACTRL_FLAG_CIF;
					codec->addSetting("frametype", "CIF");	// FIXME
				}
			}
		}
	}
	if(rtpManager != NULL)
		rtpManager->payloadTypeChanged(this, pt);
}

bool MediaCtrlRtpChannel::setDirection(int direction)
{
	if((direction < MEDIACTRL_SENDRECV) || (direction > MEDIACTRL_INACTIVE))
		return false;

	this->direction = direction;

	// TODO Change RTP settings in order to actually reflect the directionality of the stream
	return true;
}

string MediaCtrlRtpChannel::addSetting(string value)
{
	cout << "[RTP] Adding setting: " << value << " (" << label << ")" << endl;
	// TODO Parse variable and value, and take care of the supported ones
	regex crlf, re;
	// Check if there are any spaces (X-lite does not conform to the semicolon separator standard, and we are supposed to take care of it)
	if(value.find(" ") != string::npos)
		crlf.assign(" ");
	else
		crlf.assign(";");
	cmatch matches;
	sregex_token_iterator a(value.begin(), value.end(), crlf, -1);
	sregex_token_iterator b;
	if((a == b) || (*a == "")) {	// No matches
		cout << "[RTP] \tInvalid..." << endl;
		return "";
	}
	string fmt = "", v1 = "", v2 = "";
	stringstream result;
	bool resChanged = false;
	// Iterate through all the settings
	while(1) {
		fmt = *a;
		// Check each sub-attribute (they're separated by semi columns)
		re.assign("(\\w+)|(\\w+)\\=((\\w|\\,)+)", regex_constants::icase);
		if(!regex_match(fmt.c_str(), matches, re)) {
			// Invalid attribute
			cout << "[RTP] \tInvalid attribute: " << fmt << endl;
			*a++;
			if((a == b) || (*a == ""))
				break;
			continue;	// FIXME
		}
		if(matches[1].first != matches[1].second) {	// No equal sign
			v1 = string(matches[1].first, matches[1].second);
			v2 = "";
			cout << "[RTP] \t\t" << v1 << endl;
		} else {	// Variable = value(s)
			v1 = string(matches[2].first, matches[2].second);
			v2 = string(matches[3].first, matches[3].second);
			cout << "[RTP] \t\t" << v1 << " = " << v2 << endl;
		}
		// Enforce the setting, if supported
		// TODO Involve other settings (e.g. the MPI, which we ignore currently)
		if((v1 == "QCIF") && !resChanged) {
			cout << "[RTP] \t\t\tThis is what it's going to be... (QCIF)" << endl;
			resChanged = true;
			flags = MEDIACTRL_FLAG_QCIF;
			result << "QCIF=2";	// We suggest QCIF at ~15fps
		} else if((v1 == "CIF") && !resChanged)  {
			cout << "[RTP] \t\t\tThis is what it's going to be... (CIF)" << endl;
			resChanged = true;
			flags = MEDIACTRL_FLAG_CIF;
			result << "CIF=2";	// We suggest CIF at ~15fps
		}
		*a++;
		if((a == b) || (*a == ""))
			break;
	}

	return result.str();
}

void MediaCtrlRtpChannel::setClockRate(int clockrate)
{
	this->clockrate = clockrate;
	if(media == MEDIACTRL_MEDIA_AUDIO)
		timing = (1000/(8000/clockrate))*1000;
	else if(rtpManager != NULL) {
		// Get related codec, or create a new one if necessary
		if(codec == NULL) {
			codec = rtpManager->createCodec(pt);
			if(codec == NULL)
				return;
		}
		// TODO Pass to the codec info about the new clock (for fps)
		stringstream fps;
		fps << dec << (90000/clockrate);
		codec->addSetting("fps", fps.str());
		clockrate = 90000/atoi(fps.str().c_str());
		timing = (1000/(90000/clockrate))*1000;
		cout << "[RTP] Adjusted clockrate to " << dec << clockrate << " and timing to " << dec << timing << ", to reflect the fps=" << fps.str() << endl;
		if(codec->getCodecId() == MEDIACTRL_CODEC_H264)
			flags = MEDIACTRL_FLAG_CIF;
		if(flags & MEDIACTRL_FLAG_QCIF)
			codec->addSetting("frametype", "QCIF");
		else if(flags & MEDIACTRL_FLAG_CIF)
			codec->addSetting("frametype", "CIF");
		if(!codec->hasStarted()) {
			if(codec->start() == false) {
				cout << "[RTP] Codec failed to startup..." << endl;
				delete codec;
				codec = NULL;
			}
		}
	}
}

void MediaCtrlRtpChannel::lock(void *owner)
{
	if(locked)		// Already locked
		return;
	if(owner == NULL)	// Can't lock without a locker...
		return;
	locked = true;
	lockOwner = owner;
	if(rtpManager != NULL)
		rtpManager->channelLocked(this);
}

void MediaCtrlRtpChannel::unlock(void *owner)
{
	if(!locked)		// Already unlocked
		return;
	if(owner != lockOwner)	// Unauthorized unlock
		return;
	locked = false;
	lockOwner = NULL;
	if(rtpManager != NULL)
		rtpManager->channelUnlocked(this);
}

void MediaCtrlRtpChannel::clearDtmfBuffer()
{
	mTones->enter();
	tones.clear();		// Clear bufferized DTMF tones
	mTones->leave();
}

int MediaCtrlRtpChannel::getNextDtmfBuffer()
{
	int tone = -1;
	mTones->enter();
	if(!tones.empty()) {
		tone = tones.front();
		tones.pop_front();
	}
	mTones->leave();
	return tone;
}

void MediaCtrlRtpChannel::incomingData(uint8_t *buffer, int len, bool last)
{
	if((buffer == NULL) || (len == 0))
		return;

	if(last) {	// Marker bit is on, or packet=frame, report it
		if(packets.empty()) {
			MediaCtrlFrame *frame = new MediaCtrlFrame(media, buffer, len, pt);
			frame->setAllocator(RTP);
			incomingFrame(frame);	// FIXME
		} else {	// Last packet of a series
			uint8_t *newbuffer = (uint8_t*)MCMALLOC(len, sizeof(uint8_t));
			if(!newbuffer)
				return;
			memcpy(newbuffer, buffer, len);
			packets.push_back(newbuffer);
			packetLens.push_back(len);
//			cout << "[RTP] Last packet of a series: total=" << packets.size() << endl;
			// TODO Build a single frame out of the list of packets, the others will be its 'children'
			MediaCtrlFrame *mainFrame = NULL;
			while(!packets.empty()) {
				uint8_t *tmpbuffer = packets.front();
				len = packetLens.front();
				if(mainFrame == NULL) {
					mainFrame = new MediaCtrlFrame(media, tmpbuffer, len, pt);
					mainFrame->setAllocator(RTP);
				} else{
					MediaCtrlFrame *frame = new MediaCtrlFrame(media, tmpbuffer, len, pt);
					frame->setAllocator(RTP);
					mainFrame->appendFrame(frame);
				}
				MCMFREE(tmpbuffer);	// FIXME
				tmpbuffer = NULL;
				packets.pop_front();
				packetLens.pop_front();
			}
			incomingFrame(mainFrame);	// FIXME
		}
	} else {	// Marker bit is off, there are still packets we're waiting for
		uint8_t *newbuffer = (uint8_t*)MCMALLOC(len, sizeof(uint8_t));
		if(!newbuffer)
			return;
		memcpy(newbuffer, buffer, len);
		packets.push_back(newbuffer);
		packetLens.push_back(len);
	}
}

void MediaCtrlRtpChannel::incomingFrame(MediaCtrlFrame *frame)
{
	MediaCtrlFrame *decoded = frame;
	if(codec) {
		decoded = codec->decode(frame);
		if(decoded != NULL)
			decoded->setOriginal(frame);	// FIXME Keep the original undecoded frame, packages might need it
	}
	if(decoded == NULL)
		cout << "[RTP] wrong decode!" << endl;
	if((decoded != NULL) && (rtpManager != NULL)) {
		rtpManager->incomingFrame(this, decoded);
	}
}


void MediaCtrlRtpChannel::incomingDtmf(int type)
{
	mTones->enter();
	tones.push_back(type);		// Bufferize DTMF tone
	mTones->leave();
	if(rtpManager != NULL)
		rtpManager->incomingDtmf(this, type);
}

void MediaCtrlRtpChannel::wakeUp(bool doIt)
{
	if(doIt) {
		if(active)	// Already awake
			return;
		cout << "[RTP] Going to wake up the RTP thread (" << label << ")" << endl;
		active = true;
		cond->signal(true);
	} else {
		if(!active)	// Already sleeping
			return;
		cout << "[RTP] Going to make the RTP thread sleep for a bit (" << label << ")" << endl;
		active = false;
	}
}

void MediaCtrlRtpChannel::sendFrame(MediaCtrlFrame *frame)
{
	if(frame == NULL)
		return;

	if(dstPort == 0) {	// No RTP available yet
		return;
	}
	if(locked && (frame->getOwner() != lockOwner)) {
		return;
	}
	if(frame->getMediaType() != media) {	// Make sure we don't send audio on a video channel or viceversa
		return;
	}

	if(frame->getType() == LOCKING_FRAME)
		lock(frame->getOwner());
	else if(frame->getType() == UNLOCKING_FRAME)
		unlock(frame->getOwner());

	MediaCtrlFrame *frameToSend = NULL;
	if(frame->getFormat() == pt)	// Passthrough
		frameToSend = frame;
	else {	// Encode, but only if it's a raw frame (we don't transcode)
		if(frame->getFormat() != MEDIACTRL_RAW) {
//			cout << "[RTP] Not a raw frame, dropping it... (we don't transcode in here)" << endl;
			return;		// FIXME We don't transcode, only encode (if raw) or passthrough (if already encoded)
		}
#if 0
		if((frame->getOriginal() != NULL) && (frame->getOriginal()->getFormat() == pt))		// FIXME There's an already encoded frame we might use
			frameToSend = frame->getOriginal();
		else
#endif
		if((codec != NULL) && codec->hasStarted()) { 	// Encode RAW frames to the right format
			MediaCtrlFrame *newframe = codec->encode(frame);
			if(newframe != NULL) {
				frameToSend = newframe;
//			} else {
//				cout << "[RTP] Error encoding, dropping the frame..." << endl;
			}
		} else {	// Can't encode yet, drop it
			return;
		}
	}
	if(frameToSend == NULL) {
//		cout << "[RTP] frameToSend = NULL!" << endl;
		return;
	}
	if(frameToSend->getBuffer() == NULL) {
//		cout << "[RTP] frameToSend->getBuffer() = NULL!" << endl;
		return;
	}

	// Prepare the timestamp
	struct timeval tv;
	gettimeofday(&tv, NULL);
	uint32_t now = tv.tv_sec*1000 + tv.tv_usec/1000, t = 0;
	if(lastIncomingTs == 0)
		lastIncomingTs = now-1;	// FIXME
	if(now > lastIncomingTs) {	// Update the timestamp
		t = (now-lastIncomingTs)/(timing/1000);
//		if(t == 0) {	// FIXME Ugly hack
		if(t < 5) {	// FIXME Ugly hack
			t = 1;
			num += clockrate;
			lastIncomingTs += timing/1000;
		} else {			
			num += t*clockrate;
			lastIncomingTs = now;
		}
	}

	int err = 0;
	if(media == MEDIACTRL_MEDIA_AUDIO) {
		if(t == 1)	// Easy one
			err = rtp_session_send_with_ts(rtpSession, frameToSend->getBuffer(), frameToSend->getLen(), num);
		else {	// A new burst of packets after some silence, we need to set the marker bit
			mblk_t *m = rtp_session_create_packet(rtpSession, RTP_FIXED_HEADER_SIZE, frameToSend->getBuffer(), frameToSend->getLen());
			if(m) {
				rtp_set_markbit(m, 1);
				rtp_session_sendm_with_ts(rtpSession, m, num);
			}
		}
	} else {
		if(frameToSend->getAppendedFrames() == NULL) {
			mblk_t *m = rtp_session_create_packet(rtpSession, RTP_FIXED_HEADER_SIZE, frameToSend->getBuffer(), frameToSend->getLen());
			rtp_set_markbit(m, 1);
			rtp_session_sendm_with_ts(rtpSession, m, num);
		} else {
			mblk_t *m = rtp_session_create_packet(rtpSession, RTP_FIXED_HEADER_SIZE, frameToSend->getBuffer(), frameToSend->getLen());
			if(m) {
				rtp_set_markbit(m, 0);
				rtp_session_sendm_with_ts(rtpSession, m, num);
				MediaCtrlFrames *appframes = frameToSend->getAppendedFrames();
				MediaCtrlFrame *tmp = NULL;
				while(!appframes->empty()) {
					tmp = appframes->front();
					appframes->pop_front();
					m = rtp_session_create_packet(rtpSession, RTP_FIXED_HEADER_SIZE, tmp->getBuffer(), tmp->getLen());
					if(m) {
						if(appframes->empty())	// Last frame, set the Marker Bit
							rtp_set_markbit(m, 1);
						else
							rtp_set_markbit(m, 0);
						rtp_session_sendm_with_ts(rtpSession, m, num);
					}
				}
			}
		}
	}

//	rtpManager->frameSent(this, frameToSend);
}

void MediaCtrlRtpChannel::run()
{
	alive = true;
	cout << "[RTP] Joining RTP thread (" << label << ")" << endl;
	MediaCtrlFrame *frame = NULL, *tmp = NULL;

	int err = 0;
	int headerSize = RTP_FIXED_HEADER_SIZE;
	mblk_t *m = NULL;

	int have_more = 1;
	int total = 0;
	uint32_t ts = 0;
	lastTs = 0;
	uint8_t buffer[5000], temp[5000];	// FIXME

	active = false;

	struct timeval tv;
	while(alive) {
		if(!active) {
			cout << "[RTP] Thread sleeping (" << label << ")" << endl;
//			cond->enterMutex();
			cond->wait();
//			cond->leaveMutex();
			cout << "[RTP] Thread awake (" << label << ")" << endl;
		}
		if(!alive)
			break;
		// Check if there's incoming data
		have_more = 1;
		total = 0;
		if(media == MEDIACTRL_MEDIA_AUDIO) {	// audio FIXME
			while(alive && have_more) {
				err = rtp_session_recv_with_ts(rtpSession, temp, clockrate, ts, &have_more);
				if(err > 0) {
					memcpy(buffer + total, temp, err);
					total += err;
				} else
					break;
			}
			if(alive && (total > 0))
				incomingData(buffer, total);
			ts += clockrate;
		}
	}
	cout << "[RTP] Leaving RTP thread (" << label << ")" << endl;
}
