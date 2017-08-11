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
 * \brief Mixer Control Package (draft-ietf-mediactrl-mixer-control-package-06)
 *
 * \author Lorenzo Miniero <lorenzo.miniero@unina.it>
 *
 * \ingroup packages
 * \ref packages
 */

#include "expat.h"
#include "ControlPackage.h"
#include <math.h>
#include <limits.h>


extern "C" {
#ifdef FFMPEG_ALTDIR
#include <libavformat/avformat.h>	// FFmpeg libavformat
#include <libavcodec/avcodec.h>	// FFmpeg libavcodec
#else
#include <ffmpeg/avformat.h>	// FFmpeg libavformat
#include <ffmpeg/avcodec.h>	// FFmpeg libavcodec
#endif
}
static bool ffmpeg_initialized = false;


using namespace ost;
using namespace mediactrl;


/**
* \brief Small utility to generate random strings
* \note This small helper method is used to generate random identifiers
*/
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

/// Boolean Boost-Regex parser
bool booleanValue(string value, bool *ok);
bool booleanValue(string value, bool *ok)
{
	*ok = true;
	bool result = false;
	regex re;
	re.assign("true|yes|1", regex_constants::icase);	// FIXME Should be true only?
	if(!regex_match(value.c_str(), re)) {
		re.assign("false|no|0", regex_constants::icase);	// FIXME Should be false only?
		if(!regex_match(value.c_str(), re))
			*ok = false;
		else
			result = false;
	} else
		result = true;

	return result;
}

/// Positive Integer Boost-Regex parser
uint16_t positiveInteger(string value, bool *ok);
uint16_t positiveInteger(string value, bool *ok)
{
	*ok = true;
	uint16_t result = 0;
	regex re;
	cmatch matches;
	re.assign("\\+?(\\d+)", regex_constants::icase);
	if(!regex_match(value.c_str(), matches, re))
		*ok = false;
	else {
		string match(matches[1].first, matches[1].second);
		result = atoi(match.c_str());
	}

	return result;
}


/// Helper to get complementary direction (e.g. SENDONLY --> RECVONLY) for our peer
int getReverseDirection(int direction);
int getReverseDirection(int direction)
{
	if((direction == SENDRECV) || (direction == INACTIVE))
		return direction;	// The same
	else if(direction == SENDONLY)
		return RECVONLY;
	else if(direction == RECVONLY)
		return SENDONLY;
	return -1;	// Fallback
}


/// Helper to get percent volume out of a gain (e.g. -3dB --> 50%)
int getPercentVolume(int dbGain);
int getPercentVolume(int dbGain)
{
	return (int)(100*pow(10, dbGain/10.0));
}


/**
* \brief Small utility to check if an audio frame contains just silence or not (crappy and quick)
* \note This method is supposed to return true if an audio frame only contains silence: it just checks if all the samples are (in absolute) below 3000 (~10% of 32k), so it's a very dirty hack, a placeholder for future better VAD implementations
*/
bool isSilence(MediaCtrlFrame *frame);
bool isSilence(MediaCtrlFrame *frame)
{
	if((frame == NULL) || (frame->getMediaType() != MEDIACTRL_MEDIA_AUDIO) || (frame->getBuffer() == NULL) || (frame->getFormat() != MEDIACTRL_RAW))
		return false;
	bool silence = true;
	int i=0;
	short int *buffer = (short int *)frame->getBuffer();
	short int sample = 0;
	for(i=0; i < frame->getLen(); i+=2) {
		sample = abs(*buffer);
		buffer++;
		if(sample > 3000) {
			silence = false;	// At least a sample is above our threshold, so it's not silence (FIXME)
			break;
		}
	}
	return silence;
}


/// Audio mixing type
enum mixing_types {
	/*! No mixing (used only to trigger errors) */
	MIXING_NONE = 0,
	/*! N-best/loudest (actually unused, at the moment it means "mix everything") */
	MIXING_NBEST,
	/*! Controlled, for example by an external floor control (unavailable at the moment) */
	MIXING_CONTROLLER,
};

/// Volume type
enum volume_types {
	/*! Default (error handling) */
	VOLUME_NONE = -4,
	/*! Keep previous volume */
	VOLUME_KEEP = -3,
	/*! Mute */
	VOLUME_MUTE = -2,
	/*! Unmute */
	VOLUME_UNMUTE = -1,
	// All the other values are explicit percent values
};

enum subscription_types {
	/*! No subscription (used only to trigger errors) */
	SUBSCRIBE_NONE = 0,
	/*! active-talkers-sub */
	SUBSCRIBE_ACTIVETALKERS,
};	

/// unjoin-notify::status
enum {
	/*! unjoin request */
	UNJOIN_REQUEST = 0,
	/*! internal error */
	UNJOIN_ERROR,
	/*! connection/conference termination */
	UNJOIN_CONTERMINATE,
};


/// conferenceexit::status
enum {
	/*! destroyconference request */
	CONFEXIT_REQUEST = 0,
	/*! internal error */
	CONFEXIT_ERROR,
	/*! conference duration expired */
	CONFEXIT_EXPIRED,
};


// eXpat parser callbacks for parsing msc-mixer XML blobs
static void XMLCALL startElement(void *msg, const char *name, const char **atts);
static void XMLCALL valueElement(void *msg, const XML_Char *s, int len);
static void XMLCALL endElement(void *msg, const char *name);

class MixerPackage;
class MixerConference;

/// Extension to the base Stream class (msc-mixer)
class MixerStream : public MediaStream {
	public:
		MixerStream() {
			media = "";
			mediaType = MEDIACTRL_MEDIA_UNKNOWN;
			label = "";
			direction = SENDRECV;
			volume = VOLUME_KEEP;
			clamp_tones = "";
			region = "";
			priority = "";
		}
		~MixerStream() {};
	
		int volume;
	
		string clamp_tones;
		
		string region;
		string priority;
};

/// MixerPackage incoming CONTROL message
class MixerMessage : public gc {
	public:
		MixerMessage();
		~MixerMessage();

		void error(int code, string body="");
		void parseStreams();

		bool scanonly;	// true if we are only scanning the XML
		bool stop;

		MixerPackage *pkg;
		string tid;
		string blob;
		void *requester;

		string request;
		int level;
		list<string> childs;
		MixerConference *conf;
		bool newconf;

		string id1, id2;
		int id1type, id2type;
		ControlPackageConnection *con1, *con2;
		ControlPackageConnection *audioNode[2];
		int audioDirection;
		int audioVolume[2];

		list<MixerStream *> streams;

		list<Subscription *> subscriptions;

		// Audit only
		bool auditCapabilities, auditMixers;
		string auditConference;
};


/// An abstract class which acts as a linking node between connections/conferences: join/modifyjoin/unjoin+stream end up here
class MixerNode : public gc {
	public:
		MixerNode() {}
		virtual ~MixerNode() {}
	
		bool checkSender(void *sender) { return (requester == sender); };

		ControlPackageConnection *getConnection() { return connection; };
		string getConId() { return Id; };
		string getLabel() { return label; };
		int getMediaType() { return mediaType; };

		MixerPackage *getPackage() { return pkg; };
		
		virtual bool check(MixerNode *node) = 0;
		virtual int attach(MixerNode *node, int direction, int volume, int region=0, int priority=0) = 0;
		virtual int modify(MixerNode *node, int direction, int volume, int region=0, int priority=0) = 0;
		virtual int detach(MixerNode *node) = 0;

		virtual void feedFrame(MixerNode *sender, MediaCtrlFrame *frame) = 0;
		virtual void sendFrame(MixerNode *node, MediaCtrlFrame *frame) = 0;
		virtual void frameSent(MediaCtrlFrame *frame) = 0;
		virtual void incomingFrame(MediaCtrlFrame *frame) = 0;
		virtual void incomingDtmf(int type) = 0;

		MediaCtrlFrame *getLatestAudioFrame() { return NULL; };

		map<MixerNode*, int>nodes;			// All the nodes this node is attached to, and the relative direction (FIXME)
		map<MixerNode*, int>volumes;		// All the nodes this node is attached to, and the relative volumes (FIXME)
		map<MixerNode*, int>mutes;			// All the nodes this node is attached to, and the relative mutes (FIXME)

	protected:
		MixerPackage *pkg;
		void *requester;
		ControlPackageConnection *connection;		// The connection this node refers to (from~to or confid)
		string Id;									// The connection identifier (from~to or confid)
		string label;								// Only used if this is a connection

		int mediaType;
};

/// The class handling connections
class MixerConnection : public MixerNode {
	public:
		MixerConnection();
		~MixerConnection();
		
		bool setup(MixerPackage *pkg, void *requester, ControlPackageConnection *connection, ControlPackageConnection *masterConnection);

		ControlPackageConnection *getMasterConnection() { return masterConnection; };

		bool check(MixerNode *node);
		int attach(MixerNode *node, int direction, int volume, int region=0, int priority=0);
		int modify(MixerNode *node, int direction, int volume, int region=0, int priority=0);
		int detach(MixerNode *node);

		void feedFrame(MixerNode *sender, MediaCtrlFrame *frame);
		void sendFrame(MixerNode *node, MediaCtrlFrame *frame);
		void frameSent(MediaCtrlFrame *frame) { return; };			// Unused
		void incomingFrame(MediaCtrlFrame *frame);
		void incomingDtmf(int type) { return; };					// Unused

	private:
		ControlPackageConnection *masterConnection;	// The connection this node belongs to (from~to)
		int receiving;								// If this node is already receiving a stream from a node, any further attach trying
													//		to do the same will fail (i.e. we don't do any implicit mixing... FIXME)
		int sending;								// Remember if this node is sending its frames around
};


/// MixerConference
class MixerConference : public MixerNode, public Thread {
	public:
		MixerConference(MixerPackage *pkg, void *requester, string confId="");
		~MixerConference();
		
		int setup(MixerPackage *pkg, void *requester, ControlPackageConnection *connection, ControlPackageConnection *masterConnection) { return 0; };

		void subscribe(int eventType, uint32_t interval);
		void notifyEvent(string body);

		bool check(MixerNode *node);
		int attach(MixerNode *node, int direction, int volume, int region=0, int priority=0);
		int modify(MixerNode *node, int direction, int volume, int region=0, int priority=0);
		int detach(MixerNode *node);

		void feedFrame(MixerNode *sender, MediaCtrlFrame *frame);
		// Frames to be sent to all peers (e.g. announcements, generated DTMF tones, etc)
		void sendFrame(MixerNode *node, MediaCtrlFrame *frame);
		// We use this to pass the mixed output to other packages (e.g. IVR)
		void incomingFrame(MediaCtrlFrame *frame);
		void frameSent(MediaCtrlFrame *frame) { return; };			// Unused
		void incomingDtmf(int type) { return; };					// Unused

		void setAudioMixingType(int type, uint16_t n=0) { this->mixingtype = type; this->mixn = n; };
		void setReservedTalkers(uint16_t talkers) { this->talkers = talkers; };
		void setReservedListeners(uint16_t listeners) { this->listeners = listeners; };
		int getAudioMixingType() { return mixingtype; };
		uint16_t getAudioMixingN() { return mixn; };
		int getReservedTalkers() { return talkers; };
		int getReservedListeners() { return talkers; };

	private:
		void run();

		int type;

		bool audio;
		bool started, running;

		int mixingtype;		// Audio mixing type
		uint16_t mixn;		// N in case of n-best
		uint16_t talkers;		// Number of reserved talkers
		uint16_t listeners;		// Number of reserved listeners
		
		bool notifyTalkers;
		uint32_t notifyTalkersInterval;
		TimerPort *notifyTalkersTimer;
		uint32_t notifyTalkersStart;

		map<MixerNode *, MediaCtrlFrames> queuedFrames;
		ost::Mutex mPeers;
		map<string, MediaCtrlFrames> botFrames;
};


/// Audio Conferencing (msc-mixer) package class
class MixerPackage : public ControlPackage {
	public:
		MixerPackage();
		~MixerPackage();

		bool setup();
		string getInfo();

		void control(void *requester, string tid, string blob);

		void setCollector(void *frameCollector);

		void sendFrame(ControlPackageConnection *connection, MediaCtrlFrame *frame);
		void incomingFrame(ControlPackageConnection *connection, ControlPackageConnection *subConnection, MediaCtrlFrame *frame);
		void incomingDtmf(ControlPackageConnection *connection, ControlPackageConnection *subConnection, int type);
		void frameSent(ControlPackageConnection *connection, ControlPackageConnection *subConnection, MediaCtrlFrame *frame);
		void connectionLocked(ControlPackageConnection *connection, ControlPackageConnection *subConnection);
		void connectionUnlocked(ControlPackageConnection *connection, ControlPackageConnection *subConnection);
		void connectionClosing(ControlPackageConnection *connection, ControlPackageConnection *subConnection);

		void handleControl(MixerMessage *msg);
		void endConference(string confId);
		void endParticipant(string confId);

		int detach(MixerNode *who, MixerNode *peer);	// FIXME
		
		void notifyUnjoin(void *requester, string id1, string id2, int reason);
		void notifyConferenceExit(void *requester, string confid, int reason);

		list<MixerMessage *> messages;
		map<string, MixerConference *>confs;
		map<ControlPackageConnection *, MixerNode *>nodes;
		list<string>joins;

	private:
		void run();
		bool alive;
};


// ControlPackage Class Factories
extern "C" ControlPackage* create(ControlPackageCallback *callback) {
	MixerPackage *pkg = new MixerPackage();
	pkg->setCallback(callback);
	return pkg;
}

extern "C" void destroy(ControlPackage* p) {
	delete p;
}


// The MixerMessage Class (used for transactions handling)
MixerMessage::MixerMessage()
{
	scanonly = false;
	stop = false;
	pkg = NULL;
	tid = "";
	blob = "";
	requester = NULL;
	level = 0;
	childs.clear();
	conf = NULL;
	newconf = true;
	streams.clear();
	subscriptions.clear();

	auditCapabilities = true;
	auditMixers = true;
	auditConference = "";
	
	id1 = "";
	id2 = "";
	audioDirection = SENDRECV;
	audioNode[0] = audioNode[1] = NULL;
	audioVolume[0] = audioVolume[1] = VOLUME_NONE;
	con1 = con2 = NULL;
}

MixerMessage::~MixerMessage()
{
	if(!streams.empty()) {
		while(!streams.empty()) {
			MixerStream *stream = streams.front();
			streams.pop_front();
			delete stream;
		}
	}
	if(!subscriptions.empty()) {
		while(!subscriptions.empty()) {
			Subscription *subscription = subscriptions.front();
			subscriptions.pop_front();
			delete subscription;
		}
	}
}

void MixerMessage::error(int code, string body)
{
	stop = true;
	string reason;
	switch(code) {
		case 200:
			reason = "OK";
			break;
		case 400:
			reason = "Syntax error";
			break;
		case 401:
			reason = "";	// Reserved for future use
			break;
		case 402:
			reason = "";	// Reserved for future use
			break;
		case 403:
			reason = "";	// Reserved for future use
			break;
		case 404:
			reason = "";	// Reserved for future use
			break;
		case 405:
			reason = "Conference already exists";
			break;
		case 406:
			reason = "Conference does not exist";
			break;
		case 407:
			reason = "Incompatible stream configuration";
			break;
		case 408:
			reason = "joining entities already joined";
			break;
		case 409:
			reason = "joining entities not joined";
			break;
		case 410:
			reason = "Unable to join - conference full";
			break;
		case 411:
			reason = "Unable to perform join mixer operation";
			break;
		case 412:
			reason = "Connection does not exist";
			break;
		case 413:
			reason = "";	// Reserved for future use
			break;
		case 414:
			reason = "";	// Reserved for future use
			break;
		case 415:
			reason = "";	// Reserved for future use
			break;
		case 416:
			reason = "";	// Reserved for future use
			break;
		case 417:
			reason = "";	// Reserved for future use
			break;
		case 418:
			reason = "";	// Reserved for future use
			break;
		case 419:
			reason = "Other execution error";
			break;
		case 420:
			reason = "Conference reservation failed";
			break;
		case 421:
			reason = "Unable to configure audio mix";
			break;
		case 422:		// TODO When is this involved?
			reason = "Unsupported media stream configuration";
			break;
		case 423:
			reason = "Unable to configure video layouts";
			break;
		case 424:
			reason = "Unable to configure video switch";
			break;
		case 425:
			reason = "Unable to configure codecs";
			break;
		case 426:
			reason = "Unable to join - mixing connections not supported";
			break;
		case 427:
			reason = "Unable to join - mixing conferences not supported";
			break;
		case 428:
			reason = "Unsupported foreign namespace attribute or element";
			break;
		case 429:
			reason = "";	// Reserved for future use
			break;
		case 430:
			reason = "";	// Reserved for future use
			break;
		case 431:
			reason = "";	// Reserved for future use
			break;
		case 432:
			reason = "";	// Reserved for future use
			break;
		case 433:
			reason = "";	// Reserved for future use
			break;
		case 434:
			reason = "";	// Reserved for future use
			break;
		case 435:
			reason = "Other unsupported capability";
			break;
		default:
			reason = "You guess!";
			break;
	}
	if(body != "")
		reason += ": " + body;
	stringstream newblob;
	newblob << "<response status=\"";
	newblob << code << "\"";
	newblob << " reason=\"" << reason;
	newblob << "\"/>";
	pkg->callback->report(pkg, requester, tid, 200, 10, newblob.str());
}

void MixerMessage::parseStreams()
{
	// This is shared between join, modifyjoin and unjoin, so it's all part of a subroutine
	con1 = NULL;
	con2 = NULL;
	audioDirection = SENDRECV;
	audioNode[0] = audioNode[1] = NULL;
	audioVolume[0] = audioVolume[1] = VOLUME_NONE;
	con1 = pkg->callback->getConnection(pkg, id1);
	con2 = pkg->callback->getConnection(pkg, id2);
	// First of all, check if this is an authorized operation
	if(con1->getType() == CPC_CONFERENCE) {
		MixerConference *conf = (MixerConference*)pkg->nodes[con1];
		if((conf == NULL) || !conf->checkSender(requester)) {
			if(conf == NULL)
				pkg->nodes.erase(con1);
			pkg->callback->report(pkg, requester, tid, 403, 0);			// FIXME
			stop = true;
			return;
		}
	}
	if(con2->getType() == CPC_CONFERENCE) {
		MixerConference *conf = (MixerConference*)pkg->nodes[con2];
		if((conf == NULL) || !conf->checkSender(requester)) {
			if(conf == NULL)
				pkg->nodes.erase(con2);
			pkg->callback->report(pkg, requester, tid, 403, 0);			// FIXME
			stop = true;
			return;
		}
	}
	if(streams.empty()) {	// No <stream> element was provided, try attaching directly the two ids
		// Setup id1
		if(con1->getLabel() == "") {	// This is a macroconnection (from~to)
			if(con1->getType() == CPC_CONNECTION) {
				audioNode[0] = pkg->callback->getSubConnection(con1, MEDIACTRL_MEDIA_AUDIO);
				if(audioNode[0] == NULL) {	// This macroconnection has no media
					error(426, "id1 has no media connections");
					return;
				}
			} else if(con1->getType() == CPC_CONFERENCE) {
				audioNode[0] = con1;
			}
		} else {	// This is a subconnection (from~to/label)
			if(con1->getMediaType() == MEDIACTRL_MEDIA_AUDIO)
				audioNode[0] = con1;
		}
		// Setup id2
		if(con2->getLabel() == "") {	// This is a macroconnection (from~to)
			if(con2->getType() == CPC_CONNECTION) {
				audioNode[1] = pkg->callback->getSubConnection(con2, MEDIACTRL_MEDIA_AUDIO);
				if(audioNode[1] == NULL) {	// This macroconnection has no media
					error(426, "id1 has no media connections");
					return;
				}
			} else if(con2->getType() == CPC_CONFERENCE) {
				audioNode[1] = con2;
			}
		} else {	// This is a subconnection (from~to~label)
			if(con1->getMediaType() == MEDIACTRL_MEDIA_AUDIO)
				audioNode[1] = con2;
		}
	} else {	// One or more <stream> elements are involved, parse them
		MixerStream *stream = NULL;
		list<MixerStream*>::iterator iter;
		ControlPackageConnection *tmp = NULL;
		for(iter = streams.begin(); iter != streams.end(); iter++) {
			stream = (*iter);
			if(stream == NULL)
				continue;
			if(stream->label != "") {	// A label was specified, check if it's a valid one
				tmp = NULL;
				bool ok = false;
				if(((con1->getLabel() != "") && (con1->getLabel() == stream->label)) ||
					((con2->getLabel() != "") && (con2->getLabel() == stream->label))) {
						// The specified label is part of id1/id2
						if(con1->getLabel() == stream->label) {
							if(con1->getMediaType() != stream->mediaType) {
								error(407, "Invalid label " + stream->label + " media type");
								return;
							}
						} else if(con2->getLabel() == stream->label) {
							if(con2->getMediaType() != stream->mediaType) {
								error(407, "Invalid label " + stream->label + " media type");
								return;
							}
						}
						ok = true;
				}
				if(!stop && !ok) {	//
					// Start by checking id1
					tmp = pkg->callback->getSubConnection(con1, stream->label);
					if((con1->getType() == CPC_CONNECTION) && (tmp != NULL)) {	// The label exists, now check if it's compliant with the media
						if(tmp->getMediaType() != stream->mediaType) {
							error(407, "Invalid label " + stream->label + " media type");
							return;
						}
					} else {
						tmp = pkg->callback->getSubConnection(con2, stream->label);
						if((con2->getType() == CPC_CONNECTION) && (tmp != NULL)) {	// The label exists, now check if it's compliant with the media
							if(tmp->getMediaType() != stream->mediaType) {
								error(407, "Invalid label " + stream->label + " media type");
								return;
							}
						} else {	// If we're here, it means that neither id1 nor id2 know anything about the label
							error(407, "Invalid label " + stream->label);
							return;
						}
					}
				}
			}
			if(!stop) {		// No error was met in the previous steps, go on...
				// Now use the media attribute to enforce the join
				// Start with id1...
				if(con1->getMediaType() == MEDIACTRL_MEDIA_UNKNOWN) {
					if(con1->getType() == CPC_CONNECTION)
						tmp = pkg->callback->getSubConnection(con1, stream->mediaType);
					else
						tmp = con1;	// TODO Check if the conference actually supports both media
				} else
					tmp = con1;
				if(tmp == NULL) {
					error(407, id1 + " has no media " + stream->media);
					return;
				} else {	// TODO Should check if a previous <stream> already touched something here
					if(stream->mediaType == MEDIACTRL_MEDIA_AUDIO) {
						audioNode[0] = tmp;
						switch(stream->direction) {
							case SENDRECV:
								if((audioVolume[0] != VOLUME_NONE) || (audioVolume[1] != VOLUME_NONE)) {
									error(422, "audio volume conflict (SENDRECV)");
									return;										
								}
								audioDirection = SENDRECV;
								audioVolume[0] = audioVolume[1] = stream->volume;
								break;
							case SENDONLY:
								if(audioVolume[0] != VOLUME_NONE) {
									error(422, "audio volume conflict (SENDONLY)");
									return;										
								}
								audioVolume[0] = stream->volume;
								if(audioDirection == RECVONLY)
									audioDirection = SENDRECV;
								else if(audioDirection == SENDRECV)
									audioDirection = SENDONLY;
								break;
							case RECVONLY:
								if(audioVolume[1] != VOLUME_NONE) {
									error(422, "audio volume conflict (RECVONLY)");
									return;										
								}
								audioVolume[1] = stream->volume;
								if(audioDirection == SENDONLY)
									audioDirection = SENDRECV;
								else if(audioDirection == SENDRECV)
									audioDirection = RECVONLY;
								break;
							case INACTIVE:
								if((audioVolume[0] != VOLUME_NONE) || (audioVolume[1] != VOLUME_NONE)) {
									error(422, "audio volume conflict (INACTIVE)");
									return;										
								}
								audioDirection = INACTIVE;
								audioVolume[0] = audioVolume[1] = VOLUME_KEEP;	// FIXME
								break;
							default:
								cout << "[MIXER] Unknown stream->direction=" << dec << stream->direction << "... (audio)" << endl;
								break;
						}
					}
				}
				// ... and then id2 (just node assignment, volumes and so on have already been taken care of)
				if(con2->getMediaType() == MEDIACTRL_MEDIA_UNKNOWN) {
					if(con2->getType() == CPC_CONNECTION)
						tmp = pkg->callback->getSubConnection(con2, stream->mediaType);
					else
						tmp = con2;	// TODO Check if the conference actually supports both media
				} else
					tmp = con2;
				if(tmp == NULL) {
					error(407, id2 + " has no media " + stream->media);
					return;
				} else {	// TODO Should check if a previous <stream> already touched something here
					if(stream->mediaType == MEDIACTRL_MEDIA_AUDIO)
						audioNode[1] = tmp;
				}
			}
		}
	}
	cout << "[MIXER] Streams parsed:" << endl;
	cout << "[MIXER] Audio Direction: ";
	if(audioDirection == SENDRECV)
		cout << "SENDRECV" << endl;
	else if(audioDirection == SENDONLY)
		cout << "SENDONLY" << endl;
	else if(audioDirection == RECVONLY)
		cout << "RECVONLY" << endl;
	else if(audioDirection == INACTIVE)
		cout << "INACTIVE" << endl;
	cout << "[MIXER]       id1: " << id1 << endl;
	cout << "[MIXER]         A: " << (audioNode[0] ? (audioNode[0]->getConnectionId() + "/" + audioNode[0]->getLabel()) : "(none)") << endl;
	cout << "[MIXER]       id2: " << id2 << endl;
	cout << "[MIXER]         A: " << (audioNode[1] ? (audioNode[1]->getConnectionId() + "/" + audioNode[1]->getLabel()) : "(none)") << endl;
	cout << "[MIXER]       Volume in conference:   " << dec << audioVolume[0] << " <--> " << dec << audioVolume[1] << endl;
}


// The MixerNode class (used for implementing links between connections and their policies)
MixerConnection::MixerConnection()
{
	pkg = NULL;
	connection = NULL;
	masterConnection = NULL;
	Id = "";
	label = "";
	mediaType = MEDIACTRL_MEDIA_UNKNOWN;
	nodes.clear();
	volumes.clear();
	mutes.clear();
	receiving = sending = 0;
}

MixerConnection::~MixerConnection()
{
	// TODO
	connection = NULL;
	cout << "[MIXER] Removing node connection " << Id << endl;
	if(nodes.empty())
		return;
	map<MixerNode*, int>::iterator iter;
	MixerNode *node = NULL;
	while(!nodes.empty()) {
		iter = nodes.begin();
		node = iter->first;
		if(node != NULL) {
			if(node != this) {
				iter->first->detach(this);
				pkg->notifyUnjoin(requester, Id, node->getConId(), UNJOIN_CONTERMINATE);
			}
		}
		nodes.erase(iter);
	}
}

bool MixerConnection::setup(MixerPackage *pkg, void *requester, ControlPackageConnection *connection, ControlPackageConnection *masterConnection)
{
	if((pkg == NULL) || (connection == NULL) || (masterConnection == NULL))
		return false;
	this->pkg = pkg;
	this->requester = requester;
	this->connection = connection;
	this->masterConnection = masterConnection;
	this->Id = connection->getConnectionId();
	this->label = connection->getLabel();
	string nodeId;
	if(label == "")
		nodeId = this->Id;
	else
		nodeId = this->Id + "/" + label;
	cout << "[MIXER] Created new node for connection " << nodeId << endl;
	mediaType = connection->getMediaType();
	return true;
}

bool MixerConnection::check(MixerNode *node)
{
	if(node == NULL)
		return false;
	if(nodes.empty())
		return false;
	map<MixerNode*, int>::iterator iter;
	for(iter = nodes.begin(); iter != nodes.end(); iter++) {
		if(node == iter->first)
			return true;	// The nodes are linked
	}
	return false;	// Fallthrough
}

int MixerConnection::attach(MixerNode *node, int direction, int volume, int region, int priority)
{
	if(node == NULL)
		return -1;
	string nodeId = "";
	if(node->getLabel() == "")
		nodeId = node->getConId();
	else
		nodeId = node->getConId() + "/" + node->getLabel();
	if(!nodes.empty()) {	// Check if these nodes are already linked
		map<MixerNode*, int>::iterator iter;
		for(iter = nodes.begin(); iter != nodes.end(); iter++) {
			if(node == iter->first)
				return -2;	// They are, return the error
		}
	}
	if((receiving > 0) && ((direction == SENDRECV) || (direction == RECVONLY)))
		return -3;	// We don't support implicit mixing, and this node is already receiving by someone
	nodes[node] = direction;	// FIXME
	if((volume == VOLUME_MUTE) || (volume == VOLUME_UNMUTE)) {
		volumes[node] = 100;	// FIXME How to keep the old volume?
		cout << "[MIXER] \tpeer " << nodeId << " is " << (volume == VOLUME_MUTE ? "MUTE" : "UNMUTE") << " with node " << Id << endl;
		mutes[node] = volume;
	} else {
		if(volume >= 0)
			volumes[node] = volume;
		else
			volumes[node] = 100;	// FIXME default
		mutes[node] = VOLUME_UNMUTE;
		cout << "[MIXER] \tnode " << Id << " has volume " << dec << volumes[node] << "% with peer " << nodeId << endl;
	}
	if((direction == SENDRECV) || (direction == SENDONLY)) {
		sending++;
		connection->addPackage(pkg);
	}
	cout << "[MIXER] Attached peer " << nodeId << " to node " << Id << endl;
	// region and priority unused, of interest only to conferences
	return 0;
}

int MixerConnection::modify(MixerNode *node, int direction, int volume, int region, int priority)
{
	if(node == NULL)
		return -1;
	if(nodes.empty())
		return -2;
	string nodeId = "";
	if(node->getLabel() == "")
		nodeId = node->getConId();
	else
		nodeId = node->getConId() + "/" + node->getLabel();
	// Check if these nodes are actually linked
	map<MixerNode*, int>::iterator iter;
	for(iter = nodes.begin(); iter != nodes.end(); iter++) {
		if(node == iter->first) {	// Found, try to modify the join
			if((receiving > 0) && ((direction == SENDRECV) || (direction == RECVONLY)) && ((iter->second == INACTIVE) || (iter->second == SENDONLY)))
				return -3;	// We don't support implicit mixing, and this node is already receiving by someone
			if((sending > 0) && ((iter->second == SENDRECV) || (iter->second == SENDONLY))) {
				if((direction == RECVONLY) || (direction == INACTIVE)) {
					sending--;
					if(sending == 0)	// If this node doesn't send frames to anyone anymore, tell the core about it (i.e. no more events)
						connection->removePackage(pkg);
				}
			}
			if((volume == VOLUME_MUTE) || (volume == VOLUME_UNMUTE)) {
//				volumes[node] = 100;	// FIXME How to keep the old volume?
				cout << "[MIXER] \tpeer " << nodeId << " is now " << (volume == VOLUME_MUTE ? "MUTE" : "UNMUTE") << " with node " << Id << endl;
				mutes[node] = volume;
			} else {
				if(volume >= 0)
					volumes[node] = volume;
				mutes[node] = VOLUME_UNMUTE;	// FIXME
				cout << "[MIXER] \tnode " << Id << " has now volume " << dec << volumes[node] << "% with peer " << nodeId << endl;
			}
			if((direction == SENDRECV) || (direction == SENDONLY)) {
				sending++;
				connection->addPackage(pkg);
			}
			iter->second = direction;	// FIXME
			return 0;
		}
	}
	// If we got here, these nodes are not linked
	return -2;
	// region and priority unused, of interest only to conferences
}

int MixerConnection::detach(MixerNode *node)
{
	if(node == NULL)
		return -1;
	if(nodes.empty())
		return -2;
	// Check if these nodes are actually linked
	map<MixerNode*, int>::iterator iter;
	for(iter = nodes.begin(); iter != nodes.end(); iter++) {
		if(node == iter->first) {	// Found, try to unjoin them
			cout << "[MIXER] Detaching peer " << node->getConId() << " from node " << Id << endl;
			if((receiving > 0) && ((iter->second == SENDRECV) || (iter->second == RECVONLY)))
				receiving--;	// This was a node the instance was receiving frames from
			if((sending > 0) && ((iter->second == SENDRECV) || (iter->second == SENDONLY))) {
				sending--;
				if(sending == 0)	// If this node doesn't send frames to anyone anymore, tell the core about it (i.e. no more events)
					connection->removePackage(pkg);
			}
			nodes.erase(iter);	// FIXME
			return 0;
		}
	}
	// If we got here, these nodes are not linked
	return -2;	
}

void MixerConnection::feedFrame(MixerNode *sender, MediaCtrlFrame *frame)
{
	if(frame == NULL)
		return;
	if(nodes.empty()) {
		return;
	}
	map<MixerNode*, int>::iterator iter;	// Forward the frame to the RTP connection associated with this node
	for(iter = nodes.begin(); iter != nodes.end(); iter++) {
		if(iter->first == sender) {
			if((iter->second == SENDRECV) || (iter->second == RECVONLY))
				pkg->callback->sendFrame(connection, frame);
		}
	}
}

void MixerConnection::sendFrame(MixerNode *node, MediaCtrlFrame *frame)
{
	// TODO ??
}

void MixerConnection::incomingFrame(MediaCtrlFrame *frame)
{
	if(frame == NULL)
		return;
	if(connection == NULL)
		return;
	if(nodes.empty()) {
		return;
	}
	map<MixerNode*, int>::iterator iter;	// Send the frame to all the peer nodes, if allowed
	for(iter = nodes.begin(); iter != nodes.end(); iter++) {
		if(iter->first != NULL) {
			if((iter->second == SENDRECV) || (iter->second == SENDONLY)) {
				if(frame->getMediaType() == MEDIACTRL_MEDIA_AUDIO) {
					if(mutes[iter->first] == VOLUME_MUTE)
						continue;	// Mute with respect to this node
					int volume = volumes[iter->first];
					if(volume == 100) {
						iter->first->feedFrame(this, frame);	// No need to adapt the volume
					} else {
						short int *buffer = (short int*)frame->getBuffer();
						if(buffer == NULL)
							continue;
						long int longBuffer[160];
						short int newBuffer[160];
						int i=0;
						for(i=0; i<160; i++) {
							longBuffer[i] = buffer[i]*volume/100;
							if(longBuffer[i] > SHRT_MAX)
								longBuffer[i] = SHRT_MAX;	// TODO Update max/min for subsequent normalization instead?
							else if(longBuffer[i] < SHRT_MIN)
								longBuffer[i] = SHRT_MIN;
							newBuffer[i] = longBuffer[i];
						}
						MediaCtrlFrame *newFrame = new MediaCtrlFrame();
						newFrame->setAllocator(MIXER);
						newFrame->setBuffer((uint8_t*)newBuffer, 320);	// FIXME
						iter->first->feedFrame(this, newFrame);
					}
				}
			}
		}
	}
}


// The MixerConference Class (used for handling conferences and legs)
MixerConference::MixerConference(MixerPackage *pkg, void *requester, string confId)
{
	started = false;
	this->pkg = pkg;
	this->requester = requester;
	if(confId == "") {	// Generate a random one
		while(1) {	// which does not conflict
			confId = random_string(8);
			if(pkg->confs.find(confId) == pkg->confs.end()) {
				connection = pkg->callback->createConference(pkg, confId);
				if(connection != NULL) {
					connection->addPackage(pkg);
					break;
				}
			}
		}
	} else {	// TODO What about not random conference identifiers?
		connection = pkg->callback->createConference(pkg, confId);
		if(connection != NULL)
			connection->addPackage(pkg);		
	}
	
	mediaType = MEDIACTRL_MEDIA_UNKNOWN;	// Conferences usually support both the media
	
	Id = confId;
	label = "";
	cout << "[MIXER] New MixerConference created: " << confId << endl;
	// Set some default values
	talkers = 0;
	listeners = 0;
	mixingtype = MIXING_NONE;	// If this won't change, it will be an error
	mixn = 0;

	notifyTalkers = false;
	notifyTalkersInterval = 0;
	notifyTalkersTimer = NULL;
	notifyTalkersStart = 0;
	
	nodes.clear();
	volumes.clear();
	mutes.clear();

	pkg->confs[confId] = this;
	pkg->nodes[connection] = this;

	running = false;

	audio = true;

	if(this->requester == NULL)
		cout << "[MIXER] \t'requester' pointer is invalid, expect problems in notifications..." << endl;
//	start();
}

MixerConference::~MixerConference()
{
	if(running) {
		running = false;
		join();
	}
	cout << "[MIXER] MixerConference removed: " << Id << endl;
	mPeers.enter();
	// TODO Actually detach the conference from the node
#if 0	
	if(!nodes.empty()) {
		while(!nodes.empty()) {
			MixerNode *peer = nodes.front();
			nodes.pop_front();
		}
	}
#endif
	// Detach audio connections
	MixerNode *node = NULL;
	map<MixerNode*, int>::iterator iter;
	for(iter = nodes.begin(); iter != nodes.end(); iter++) {
		node = iter->first;
		if(node == NULL)
			continue;
		node->detach(this);	// FIXME
		pkg->notifyUnjoin(requester, Id, node->getConId(), UNJOIN_CONTERMINATE);
	}
	nodes.clear();

	// FIXME Notify conferenceexit
	if(started)
		pkg->notifyConferenceExit(requester, Id, CONFEXIT_REQUEST);	// FIXME
	
	mPeers.leave();
	pkg->endConference(Id);
	pkg->nodes.erase(connection);
}

bool MixerConference::check(MixerNode *node)
{
	if(node == NULL)
		return false;
	if(nodes.empty())
		return false;
	if(!nodes.empty()) {
		map<MixerNode*, int>::iterator iter;
		for(iter = nodes.begin(); iter != nodes.end(); iter++) {
			if(node == iter->first)
				return true;	// The nodes are linked
		}
	}
	return false;	// Fallthrough
}

int MixerConference::attach(MixerNode *peer, int direction, int volume, int region, int priority)
{
	// TODO actually attach peer...
	if(peer == NULL)
		return -1;
	string peerId = "";
	if(peer->getLabel() == "")
		peerId = peer->getConId();
	else
		peerId = peer->getConId() + "/" + peer->getLabel();
	mPeers.enter();
	if(peer->getMediaType() == MEDIACTRL_MEDIA_AUDIO) {
		cout << "[MIXER] Trying to attach audio node " << peerId << " to conference " << Id << "..." << endl;
		if(!nodes.empty()) {	// Check if these nodes are already linked
			map<MixerNode*, int>::iterator iter;
			for(iter = nodes.begin(); iter != nodes.end(); iter++) {
				if(peer == iter->first) {
					cout << "[MIXER]       FAILED! (already joined)" << endl;
					mPeers.leave();
					return -2;	// They are, return the error
				}
			}
		}
		nodes[peer] = direction;	// FIXME
		if((volume == VOLUME_MUTE) || (volume == VOLUME_UNMUTE)) {
			volumes[peer] = 100;	// FIXME How to keep the old volume?
			cout << "[MIXER] \taudio node " << peerId << " has a " << (volume == VOLUME_MUTE ? "MUTE" : "UNMUTE") << " volume from conference " << Id << endl;
			mutes[peer] = volume;
		} else {
			if(volume >= 0)
				volumes[peer] = volume;
			else
				volumes[peer] = 100;	// FIXME default
			mutes[peer] = VOLUME_UNMUTE;	// FIXME
			cout << "[MIXER] \taudio node " << peerId << " receives volume " << dec << volumes[peer] << "% from conference " << Id << endl;
		}
		cout << "[MIXER]       OK!" << endl;
		mPeers.leave();
		return 0;
	}
	mPeers.leave();
	return -1;
}

int MixerConference::modify(MixerNode *peer, int direction, int volume, int region, int priority)
{
	if(peer == NULL)
		return -1;
	string peerId = "";
	if(peer->getLabel() == "")
		peerId = peer->getConId();
	else
		peerId = peer->getConId() + "/" + peer->getLabel();
	mPeers.enter();
	if(peer->getMediaType() == MEDIACTRL_MEDIA_AUDIO) {
		cout << "[MIXER] Trying to modify audio node " << peerId << " and conference " << Id << "..." << endl;
		bool found = false;
		if(!nodes.empty()) {	// Check if these nodes are already linked
			map<MixerNode*, int>::iterator iter;
			for(iter = nodes.begin(); iter != nodes.end(); iter++) {
				if(peer == iter->first) {	// They are, modify the direction
					iter->second = direction;
					found = true;
					break;
				}
			}
		}
		if(!found) {
			mPeers.leave();
			return -2;
		}
		if(!volumes.empty()) {	// Updated volume
			map<MixerNode*, int>::iterator iter;
			for(iter = volumes.begin(); iter != volumes.end(); iter++) {
				if(peer == iter->first) {
					if(volume >= 0)
						iter->second = volume;
					found = true;
					break;
				}
			}
		}
		if(!found) {
			mPeers.leave();
			return -2;
		}
		if(!mutes.empty()) {	// Updated muted/unmuted status
			map<MixerNode*, int>::iterator iter;
			for(iter = mutes.begin(); iter != mutes.end(); iter++) {
				if(peer == iter->first) {
					if((volume == VOLUME_MUTE) || (volume == VOLUME_UNMUTE))
						iter->second = volume;
					else
						iter->second = VOLUME_UNMUTE;	// FIXME
					found = true;
					break;
				}
			}
		}
		if(!found) {
			mPeers.leave();
			return -2;
		}
		if((volume == VOLUME_MUTE) || (volume == VOLUME_UNMUTE)) {
			volumes[peer] = 100;	// FIXME How to keep the old volume?
			cout << "[MIXER] \taudio node " << peerId << " receives now " << (volume == VOLUME_MUTE ? "MUTE" : "UNMUTE") << " volume from conference " << Id << endl;
			mutes[peer] = volume;
		} else {
			if(volume >= 0)
				volumes[peer] = volume;
			mutes[peer] = VOLUME_UNMUTE;	// FIXME
			cout << "[MIXER] \taudio node " << peerId << " receives now volume " << dec << volumes[peer] << "% from conference " << Id << endl;
		}
		mPeers.leave();
		return 0;
	}
	mPeers.leave();
	return 0;
}

int MixerConference::detach(MixerNode *peer)
{
	if(peer == NULL)
		return -1;
	mPeers.enter();
	string peerId = "";
	if(peer->getLabel() == "")
		peerId = peer->getConId();
	else
		peerId = peer->getConId() + "/" + peer->getLabel();
	if(peer->getMediaType() == MEDIACTRL_MEDIA_AUDIO) {
		cout << "[MIXER] MixerConference " << Id << ", detaching audio peer: " << peerId << endl;
		if(!nodes.empty()) {	// Check if these nodes are already linked
			map<MixerNode*, int>::iterator iter;
			for(iter = nodes.begin(); iter != nodes.end(); iter++) {
				if(peer == iter->first) {	// They are, remove the node
					nodes.erase(iter);
					queuedFrames.erase(iter->first);
					mPeers.leave();
					return 0;
				}
			}
			nodes.erase(peer);
		}
		mPeers.leave();
		return -2;	// Not found
	}
	mPeers.leave();
	return 0;
}

void MixerConference::subscribe(int eventType, uint32_t interval)
{
	if(eventType != SUBSCRIBE_ACTIVETALKERS)
		return;
	this->notifyTalkersInterval = interval*1000;
	if(interval == 0) {
		notifyTalkers = false;
		if(notifyTalkersTimer != NULL)
			delete notifyTalkersTimer;
		notifyTalkersTimer = NULL;
		notifyTalkersStart = 0;
	} else {
		notifyTalkers = true;
		if(notifyTalkersTimer == NULL) {
			notifyTalkersTimer = new TimerPort();
			notifyTalkersTimer->incTimer(0);
			notifyTalkersStart = notifyTalkersTimer->getElapsed();	
		}			
	}
}

void MixerConference::notifyEvent(string body)
{
	// Use CONTROL for events notification
	stringstream event;
	event // << "<?xml version=\"1.0\"?>"
		<< "<mscmixer version=\"1.0\" xmlns=\"urn:ietf:params:xml:ns:msc-mixer\">"
		<< "<event>" << body << "</event></mscmixer>";
	// Send the event
	pkg->callback->control(pkg, requester, event.str());		// FIXME
}

void MixerConference::feedFrame(MixerNode *sender, MediaCtrlFrame *frame)
{
	// We just received a frame, decode it if needed and then queue it to mix it later
	MediaCtrlFrame *newframe = frame;
	if(frame->getFormat() != MEDIACTRL_RAW) {
		newframe = pkg->callback->decode(frame);
	}
	if(newframe) {
		mPeers.enter();
		queuedFrames[sender].push_back(newframe);
		mPeers.leave();
	}
}

void MixerConference::sendFrame(MixerNode *peer, MediaCtrlFrame *frame)
{
	if(this != peer) {
		return;
	}
	// Part of an announcement, queue this frame for all attached peers
	incomingFrame(frame);	// The sender is the conference itself, so that it is recognized as an announcement later
//	frameSent(connection, frame);
}

void MixerConference::incomingFrame(MediaCtrlFrame *frame)
{
	// We just received an announcement frame, decode it if needed and then queue it to mix it later
	MediaCtrlFrame *newframe = frame;
	if(frame->getFormat() != MEDIACTRL_RAW) {
		newframe = pkg->callback->decode(frame);
		newframe->setTransactionId(frame->getTransactionId());
	}
	if(newframe) {
		string frameTid = newframe->getTransactionId();
		if(frameTid == "")
			return;		// We don't know which transaction it refers to
		if(newframe->getType() == UNLOCKING_FRAME)
			botFrames.erase(frameTid);	// FIXME
		else
			botFrames[frameTid].push_back(newframe);
	}
}


void MixerConference::run()
{
	started = true;
	cout << "[MIXER] MixerConference thread starting: " << Id << endl;
	running = true;
	long int buffer[160], sumBuffer[160];
	short int outBuffer[160], *curBuffer = NULL;
	memset(buffer, 0, 640);
	memset(sumBuffer, 0, 640);
	memset(outBuffer, 0, 320);
	bool playingAnnouncement = false;
	map<MixerNode *, int>::iterator iter;
	MixerNode *node = NULL;
	bool silent = false;
	list<string> talkers;

	struct timeval now, before;
	gettimeofday(&before, NULL);
	now.tv_sec = before.tv_sec;
	now.tv_usec = before.tv_usec;
	time_t passed, d_s, d_us;
	int volume = 0;

	while(running) {
		talkers.clear();
		// See if it's time to prepare a frame
		gettimeofday(&now, NULL);
		d_s = now.tv_sec - before.tv_sec;
		d_us = now.tv_usec - before.tv_usec;
		if(d_us < 0) {
			d_us += 1000000;
			--d_s;
		}
		passed = d_s*1000000 + d_us;
		if(passed < 18500) {
			usleep(1000);
			continue;
		}
		// Update the reference time
		before.tv_usec += 20000;
		if(before.tv_usec > 1000000) {
			before.tv_sec++;
			before.tv_usec -= 1000000;
		}
		if(!running)
			break;
		// First sum up all buffers... (TODO handle clipping)
		memset(buffer, 0, 640);
		// ...starting with announcements, if there are any...
		if(!botFrames.empty()) {
			if(!playingAnnouncement) {
				cout << "[MIXER] Playing announcements" << endl;
				playingAnnouncement = true;
			}
			MediaCtrlFrame *frame = NULL;
			map<string, MediaCtrlFrames>::iterator iter;
			for(iter = botFrames.begin(); iter != botFrames.end(); iter++) {
				if(iter->second.empty())
					continue;
				frame = iter->second.front();
				iter->second.pop_front();
				if(frame != NULL) {
					// TODO Reimplement locking, which is broken now...
#if 0
					if(frame->getType() == LOCKING_FRAME) {
//						connection->connectionLocked();
					} else if(frame->getType() == UNLOCKING_FRAME) {
						botFrames.erase(iter);	// FIXME
//						connection->connectionUnlocked();
						break;	// FIXME FIXME FIXME
					}
#endif
					curBuffer = (short int*)frame->getBuffer();
					if(curBuffer != NULL) {
						int i=0;
						for(i=0; i<160; i++)
							buffer[i] += curBuffer[i];
					}
				}
			}
		} else {
			if(playingAnnouncement)
				playingAnnouncement = false;
		}
		// ...but stop immediately if there are no peers...
		if(nodes.empty())
			continue;
		mPeers.enter();
		for(iter = nodes.begin(); iter != nodes.end(); iter++) {
			silent = false;
			node = iter->first;
			if(node == NULL)
				continue;
			if((iter->second != SENDRECV) && (iter->second != RECVONLY))	// This participant is not allowed to contribute to the audio mixing
				continue;
			if(queuedFrames[node].empty())
				continue;
			MediaCtrlFrame *frame = queuedFrames[node].front();
			if(frame == NULL)
				continue;
			if(isSilence(frame))
				silent = true;	// This appears to be a silent frame
			curBuffer = (short int*)frame->getBuffer();
			if(curBuffer != NULL) {
				int i=0;
				if(playingAnnouncement) {
					for(i=0; i<160; i++)
						buffer[i] += (curBuffer[i]/3);
				} else {
					for(i=0; i<160; i++)
						buffer[i] += curBuffer[i];
				}
				if(!silent)
					talkers.push_back(node->getConId());
			}
		}
		// ...then prepare the mix for each participant
		for(iter = nodes.begin(); iter != nodes.end(); iter++) {
			node = iter->first;
			if(node == NULL)
				continue;
			if((iter->second != SENDRECV) && (iter->second != SENDONLY))	// This participant is not allowed to receive the audio mix
				continue;
			if(mutes[node] == VOLUME_MUTE)
				continue;	// The conference is mute (yuck) with this peer
			volume = volumes[node];
			curBuffer = NULL;	// By default we assume participants are not included in the mix, and so no echo cancellation must be done		
			if(!queuedFrames[node].empty()) {
				if((iter->second == SENDRECV) || (iter->second == RECVONLY)) {	// But this participant is, so its echo must be removed
					MediaCtrlFrame *frame = queuedFrames[node].front();
					if(frame != NULL)
						curBuffer = (short int*)frame->getBuffer();
				}
			}
			int i=0;
			memset(sumBuffer, 0, 640);
			if(playingAnnouncement) {
				for(i=0; i<160; i++) {
					sumBuffer[i] = buffer[i] - (curBuffer ? (curBuffer[i]/3) : 0);
					if(volume != 100)	// Adapt the volume
						sumBuffer[i] = sumBuffer[i]*volume/100;
					if(sumBuffer[i] > SHRT_MAX)
						sumBuffer[i] = SHRT_MAX;	// TODO Update max/min for subsequent normalization instead?
					else if(sumBuffer[i] < SHRT_MIN)
						sumBuffer[i] = SHRT_MIN;
				}
			} else {
				for(i=0; i<160; i++) {
					sumBuffer[i] = buffer[i] - (curBuffer ? (curBuffer[i]) : 0);
					if(volume != 100)	// Adapt the volume
						sumBuffer[i] = sumBuffer[i]*volume/100;
					if(sumBuffer[i] > SHRT_MAX)
						sumBuffer[i] = SHRT_MAX;	// TODO Update max for subsequent normalization instead?
					else if(sumBuffer[i] < SHRT_MIN)
						sumBuffer[i] = SHRT_MIN;
				}
			}
			memset(outBuffer, 0, 320);
			for(i=0; i<160; i++) {
				// TODO Normalize instead of truncating?
				outBuffer[i] = sumBuffer[i];
			}
			MediaCtrlFrame *newframe = new MediaCtrlFrame(MEDIACTRL_MEDIA_AUDIO, (uint8_t*)outBuffer, 320);
			newframe->setAllocator(MIXER);
			// Send this frame to the participant
			if(node != NULL)
				node->feedFrame(this, newframe);
		}
//		connection->incomingFrame(connection, connection, new MediaCtrlFrame(MEDIACTRL_MEDIA_AUDIO, (uint8_t*)buffer, 320));
		// Get rid of the old, now useless, frames
		for(iter = nodes.begin(); iter != nodes.end(); iter++) {
			node = iter->first;
			if(node == NULL)
				continue;
			if(!queuedFrames[node].empty()) {
				queuedFrames[node].pop_front();
			}
		}
		// Check if we need to trigger active-talkers-notify events
		if(notifyTalkers && (notifyTalkersTimer != NULL) && !talkers.empty()) {
			if((notifyTalkersTimer->getElapsed()-notifyTalkersStart) > notifyTalkersInterval) {
				// Prepare the event
				stringstream event;
				event << "<active-talkers-notify conferenceid=\"" << Id << "\">";
				while(1) {
					if(talkers.empty())
						break;
					event << "<active-talker connectionid=\"" << talkers.front() << "\"/>";
					// TODO Check if it's a connection or a conference...
					talkers.pop_front();
				}
				event << "</active-talkers-notify>";
				notifyEvent(event.str());
				notifyTalkersStart = notifyTalkersTimer->getElapsed();
			}
		}
		mPeers.leave();
	}
	running = false;
}


// Class Methods
MixerPackage::MixerPackage() {
	MCMINIT();
	cout << "[MIXER] Registering MixerPackage()" << endl;
	name = "msc-mixer";
	version = "1.0";
	desc = "Media Server Control - Mixer - version 1.0";
	mimeType = "application/msc-mixer+xml";
	alive = false;
}

MixerPackage::~MixerPackage() {
	cout << "[MIXER] Removing MixerPackage()" << endl;
	if(alive) {
		alive = false;
		join();
	}
}

void MixerPackage::setCollector(void *frameCollector)
{
	setCommonCollector(frameCollector);
	cout << "[MIXER] Frame Collector " << (getCollector() ? "OK" : "NOT OK :(") << endl;
}

bool MixerPackage::setup()
{
	cout << "[MIXER] Initializing ffmpeg related stuff" << endl;
	if(ffmpeg_initialized == false) {
		ffmpeg_initialized = true;
	    avcodec_register_all();
		av_register_all();
	}
	start();
	return true;
}

string MixerPackage::getInfo()
{
	stringstream info;
	info << "Running: " << alive << endl;
	info << "Conferences:" << endl;
	if(confs.empty())
		info << "\tnone" << endl;
	else {
		map<string, MixerConference *>::iterator iter;
		MixerConference *conf = NULL;
		for(iter = confs.begin(); iter != confs.end(); iter++) {
			conf = iter->second;
			if(conf == NULL)
				continue;
			info << "\t" << iter->first << endl;
		}
	}
	info << "Nodes:" << endl;
	if(nodes.empty())
		info << "\tnone" << endl;
	else {
		map<ControlPackageConnection *, MixerNode *>::iterator iter;
		MixerNode *participant = NULL;
		for(iter = nodes.begin(); iter != nodes.end(); iter++) {
			participant = iter->second;
			if(participant == NULL)
				continue;
			info << "\t" << participant->getConId() << endl;
			// TODO Add who's attached to this node
#if 0
			if(participant->getPeer() != NULL)
				info << "\t\tattached to " << participant->getPeer()->getConId() << endl;
#endif
		}
	}
	info << "Transactions:" << endl;
	if(messages.empty())
		info << "\tnone" << endl;
	else {
		list<MixerMessage *>::iterator iter;
		MixerMessage *msg = NULL;
		for(iter = messages.begin(); iter != messages.end(); iter++) {
			msg = (*iter);
			if(msg == NULL)
				continue;
			info << "\t" << msg->tid << endl;
			info << "\t\t" << msg->request << endl;
		}
	}

	return info.str();
}

void MixerPackage::control(void *requester, string tid, string blob) {
	cout << "[MIXER] \tMixerPackage() received CONTROL message (tid=" << tid << ")" << endl;
	MixerMessage *msg = new MixerMessage();
	msg->pkg = this;
	msg->tid = tid;
	msg->blob = blob;
	msg->requester = requester;
	messages.push_back(msg);
}

void MixerPackage::sendFrame(ControlPackageConnection *connection, MediaCtrlFrame *frame)
{
	if(connection == NULL) {
		return;
	}
	if(nodes.empty()) {
		return;
	}
	MixerNode *node = nodes[connection];
	if(node != NULL)
		node->sendFrame(node, frame);
	else
		nodes.erase(connection);
}

void MixerPackage::incomingFrame(ControlPackageConnection *connection, ControlPackageConnection *subConnection, MediaCtrlFrame *frame)
{
	if((connection == NULL) || (subConnection == NULL)) {
		cout << "NULL" << endl;
		return;
	}
	if(nodes.empty()) {
		cout << "nodes empty" << endl;
		return;
	}
	MixerNode *node = nodes[subConnection];
	if(node != NULL) {
		if(subConnection->getType() != CPC_CONFERENCE)	// FIXME
			node->incomingFrame(frame);
	} else
		nodes.erase(connection);
}

void MixerPackage::incomingDtmf(ControlPackageConnection *connection, ControlPackageConnection *subConnection, int type)
{
	// TODO Should we handle DTMF here? We currently don't
#if 0
	if((connection == NULL) || (subConnection == NULL))
		return;
	if(nodes.empty()) {
		return;
	}
	MixerNode *node = nodes[subConnection];
	if(node != NULL)
		node->incomingDtmf(type);
	else
		nodes.erase(connection);
#endif
}

void MixerPackage::frameSent(ControlPackageConnection *connection, ControlPackageConnection *subConnection, MediaCtrlFrame *frame)
{
#if 0
	if((connection == NULL) || (subConnection == NULL))
		return;
	if(nodes.empty()) {
		return;
	}
	MixerNode *node = nodes[subConnection];
	if(node != NULL)
		node->frameSent(frame);
	else
		nodes.erase(connection);
#endif
}

void MixerPackage::connectionLocked(ControlPackageConnection *connection, ControlPackageConnection *subConnection)
{
#if 0
	if((connection == NULL) || (subConnection == NULL))
		return;
	if(nodes.empty()) {
		return;
	}
	MixerNode *node = nodes.find(subConnection);
	if(node != NULL)
		node->connectionLocked();
#endif
}

void MixerPackage::connectionUnlocked(ControlPackageConnection *connection, ControlPackageConnection *subConnection)
{
#if 0
	if((connection == NULL) || (subConnection == NULL))
		return;
	if(nodes.empty()) {
		return;
	}
	MixerNode *node = nodes.find(subConnection);
	if(node != NULL)
		node->connectionUnlocked();
#endif
}

void MixerPackage::connectionClosing(ControlPackageConnection *connection, ControlPackageConnection *subConnection)
{
	if((connection == NULL) || (subConnection == NULL))
		return;
	if(nodes.empty())
		return;
	cout << "[MIXER] Connection closing: " << subConnection->getConnectionId() << "/" << subConnection->getLabel() << endl;
	MixerNode *node = nodes[subConnection];
	nodes.erase(subConnection);
	if(node != NULL)
//		node->connectionClosing();
		delete node;	// FIXME
	if(connection != NULL)
		connection->removePackage(this);
	if((connection != subConnection) && (subConnection != NULL))
		subConnection->removePackage(this);
}

void MixerPackage::endConference(string confId)
{
	// FIXME
//	confs[confId] = NULL;
	confs.erase(confId);
}

int MixerPackage::detach(MixerNode *who, MixerNode *peer)
{
	if(!who || !peer)
		return -1;
	who->detach(peer);	// FIXME
	peer->detach(who);	// FIXME
	// TODO notifyUnjoin?
	return 0;
}

void MixerPackage::notifyUnjoin(void *requester, string id1, string id2, int reason)
{
	if(joins.empty())
		return;
	string dummy1 = id1 + "~" + id2;
	string dummy2 = id2 + "~" + id1;
	bool found = false;
	list<string>::iterator iter;
	for(iter = joins.begin(); iter != joins.end(); iter++) {
		if(((*iter) == dummy1) || ((*iter) == dummy2)) {
			joins.erase(iter);
			found = true;
			break;
		}
	}
	if(!found)
		return;
	cout << "[MIXER] Notifying unjoin between " << id1 << " and " << id2 << " (reason=" << dec << reason << ")" << endl;
	// Use CONTROL for events notification
	stringstream event;
	event //<< "<?xml version=\"1.0\"?>"
		<< "<mscmixer version=\"1.0\" xmlns=\"urn:ietf:params:xml:ns:msc-mixer\">"
		<< "<event>"
		<< "<unjoin-notify status=\"" << dec << reason << "\" id1=\"" << id1 << "\" id2=\"" << id2 << "\"/>"
		<< "</event></mscmixer>";
	// Send the event
	callback->control(this, requester, event.str());		// FIXME
}

void MixerPackage::notifyConferenceExit(void *requester, string confid, int reason)
{
	cout << "[MIXER] Notifying conference exit (" << confid << ", reason=" << dec << reason << ")" << endl;
	// Use CONTROL for events notification
	stringstream event;
	event //<< "<?xml version=\"1.0\"?>"
		<< "<mscmixer version=\"1.0\" xmlns=\"urn:ietf:params:xml:ns:msc-mixer\">"
		<< "<event>"
		<< "<conferenceexit status=\"" << dec << reason << "\" conferenceid=\"" << confid << "\"/>"
		<< "</event></mscmixer>";
	// Send the event
	callback->control(this, requester, event.str());		// FIXME
}

void MixerPackage::run() {
	cout << "[MIXER] Joining MixerPackage->thread()" << endl;
	bool waiting = false;
	alive = true;
	while(alive) {
		if(waiting) {
			struct timeval tv = {0, 50000};
			select(0, NULL, NULL, NULL, &tv);
		}
		if(messages.empty()) {
			waiting = true;
			continue;
		}
		waiting = false;
		MixerMessage *msg = messages.front();
		messages.pop_front();
		cout << "[MIXER] \tHandling CONTROL message (tid=" << msg->tid << ")" << endl;
		handleControl(msg);
		cout << "[MIXER] Conf->thread() back to sleep..." << endl;
	}
	cout << "[MIXER] Leaving Conf->thread()" << endl;
}

void MixerPackage::handleControl(MixerMessage *msg) {
	cout << "[MIXER] \tMixerPackage->control (tid=" << msg->tid << ")" << endl;
	if(msg->blob == "") {
		cout << "[MIXER]     Bodyless CONTROL" << endl;
		// CONTROL without body?
		msg->error(400, "No XML body");	// FIXME
		delete msg;
		return;
	}
	// First scan the XML blob for errors...
	msg->scanonly = true;
	XML_Parser parser = XML_ParserCreate(NULL);
	XML_SetUserData(parser, msg);
	XML_SetElementHandler(parser, startElement, endElement);
	if (XML_Parse(parser, msg->blob.c_str(), msg->blob.length(), 1) == XML_STATUS_ERROR) {
		cout << "[MIXER]     Error parsing MixerPackage CONTROL body: '"
			<< XML_ErrorString(XML_GetErrorCode(parser))
			<< "' at " << XML_GetCurrentLineNumber(parser) << ":"
			<< XML_GetCurrentColumnNumber(parser) << endl;
		cout << "[MIXER]     Broken body was:" << endl << msg->blob.c_str() << "(" << dec << msg->blob.length() << ")" << endl;
		msg->error(400, "Invalid XML body");	// FIXME
		delete msg;
		return;
	}
	XML_ParserFree(parser);
	// ... and then actually parse and handle it
	msg->scanonly = false;
	parser = XML_ParserCreate(NULL);
	XML_SetUserData(parser, msg);
	XML_SetElementHandler(parser, startElement, endElement);
	XML_SetCharacterDataHandler(parser, valueElement);
	XML_Parse(parser, msg->blob.c_str(), msg->blob.length(), 1);
	XML_ParserFree(parser);
	delete msg;
}


// eXpat callbacks handle the parsing of CONTROL bodies
static void XMLCALL startElement(void *msg, const char *name, const char **atts)
{
	MixerMessage *message = (MixerMessage *)msg;
	if(message->scanonly)
		return;
	if(message->stop)
		return;
	if(message->level == 0) {
		// FIXME do some initialization here? it's the root
	}
	message->level++;
	message->childs.push_back(name);
	if(message->level == 1) {
		if(message->childs.back() != "mscmixer") {	// mscmixer, root of all requests/responses/notifications
			message->error(400, "mscmixer");
			return;
		}
		if(!atts) {	// version was not specified
			message->error(400, "version");
			return;
		}
		string version = "", nameSpace = "";
		int i = 0;
		while(atts[i]) {	// We don't care about other attributes
			if(!atts[i])
				break;
			if(!strcmp(atts[i], "version"))
				version = atts[i+1];
			else if(!strcmp(atts[i], "xmlns"))
				nameSpace = atts[i+1];
			else {
				message->error(428, atts[i]);
				return;
			}
			i += 2;
		}
		if(version == "") {
			message->error(400, "version");
			return;
		}
		if(version != "1.0") {
			message->error(400, "version is not 1.0");
			return;
		}
		if((nameSpace == "") || (nameSpace != "urn:ietf:params:xml:ns:msc-mixer")) {
			message->error(428, "Invalid mscmixer namespace");
			return;
		}
	} else if(message->level == 2) {
		// Handle possible elements at level 2 according to level 1
		message->request = name;
		if(message->childs.back() == "createconference") {
			message->newconf = true;
			if(!atts) {	// conferenceid was not specified
				message->conf = new MixerConference(message->pkg, message->requester);
			} else {	// conferenceid was specified, check if it already exists
				string confId = "";
				uint16_t rTalkers = 0, rListeners = 0;
				int i = 0;
				while(atts[i]) {	// We don't care about other attributes
					if(!atts[i] || confId != "")
						break;
					if(!strcmp(atts[i], "conferenceid")) {
						confId = atts[i+1];
						break;
					} else if(!strcmp(atts[i], "reserved-talkers")) {
						bool ok = false;
						rTalkers = positiveInteger(atts[i+1], &ok);
						if(!ok) {		// Invalid value for attribute
							message->error(420, "reserved-talkers");
							return;
						}
						break;
					} else if(!strcmp(atts[i], "reserved-listeners")) {
						bool ok = false;
						rListeners = positiveInteger(atts[i+1], &ok);
						if(!ok) {		// Invalid value for attribute
							message->error(420, "reserved-listeners");
							return;
						}
						break;
					}
					i += 2;
				}
				if(message->pkg->callback->getConnection(message->pkg, confId) != NULL) {	// This identifier already exists
					message->error(405, confId);
					return;
				}
				if(message->pkg->confs[confId] != NULL) {	// We already handle a conference with this identifier
					message->error(405, confId);
					return;
				} else {
					message->conf = new MixerConference(message->pkg, message->requester, confId);
					message->conf->setReservedTalkers(rTalkers);
					message->conf->setReservedListeners(rListeners);
				}
			}
		} else if(message->childs.back() == "modifyconference") {
			message->newconf = false;
			if(!atts) {	// conferenceid was not specified
				message->error(400, "conferenceid");
				return;
			} else {	// conferenceid was specified, remove it
				const char *confId = NULL;
				int i = 0;
				while(atts[i]) {	// We don't care about other attributes
					if(!atts[i] || confId)
						break;
					if(!strcmp(atts[i], "conferenceid")) {
						confId = atts[i+1];
						break;
					}
					i += 2;
				}
				if(!confId)
					confId = "";
				if(confId == "") {	// conferenceid is mandatory
					message->error(400, "conferenceid");
					return;
				}
				if(message->pkg->confs[confId] == NULL) {
					message->error(406, confId);
					return;
				} else {
					message->conf = message->pkg->confs[confId];
					if(!message->conf->checkSender(message->requester)) {	// Unauthorized: not the original requester
						message->pkg->callback->report(message->pkg, message->requester, message->tid, 403, 0);			// FIXME
						message->stop = true;
						return;
					}
				}
			}
		} else if(message->childs.back() == "destroyconference") {
			message->newconf = false;
			if(!atts) {	// conferenceid was not specified
				message->error(400, "conferenceid");
				return;
			} else {	// conferenceid was specified, remove it
				const char *confId = NULL;
				int i = 0;
				while(atts[i]) {	// We don't care about other attributes
					if(!atts[i] || confId)
						break;
					if(!strcmp(atts[i], "conferenceid")) {
						confId = atts[i+1];
						break;
					}
					i += 2;
				}
				if(!confId)
					confId = "";
				if(confId == "") {	// conferenceid is mandatory
					message->error(400, "conferenceid");
					return;
				}
				if(message->pkg->confs[confId] == NULL) {
					message->error(406, confId);
					return;
				} else {
					// Remove conference later
					message->conf = message->pkg->confs[confId];
					if(!message->conf->checkSender(message->requester)) {	// Unauthorized: not the original requester
						message->pkg->callback->report(message->pkg, message->requester, message->tid, 403, 0);			// FIXME
						message->stop = true;
						return;
					}
				}
			}
		} else if(message->childs.back() == "join") {
			if(!atts) {	// id1 and id2 were not specified
				message->error(400, "id1, id2");
				return;
			}
			int i = 0;
			while(atts[i]) {
				if(!atts[i])
					break;
				if(!strcmp(atts[i], "id1"))
					message->id1 = atts[i+1];
				else if(!strcmp(atts[i], "id2"))
					message->id2 = atts[i+1];
				else {
					message->error(428, atts[i]);
					return;
				}
				i += 2;
			}
			if(message->pkg->callback->getConnection(message->pkg, message->id1) == NULL) {
				message->error(((message->id1.find("~") != string::npos) ? 412 : 406), message->id1 + " does not exist");
				return;
			}
			if(message->pkg->callback->getConnection(message->pkg, message->id2) == NULL) {
				message->error(((message->id2.find("~") != string::npos) ? 412 : 406), message->id2 + " does not exist");
				return;
			}
		} else if(message->childs.back() == "modifyjoin") {
			if(!atts) {	// id1 and id2 were not specified
				message->error(400, "id1, id2");
				return;
			}
			int i = 0;
			while(atts[i]) {
				if(!atts[i])
					break;
				if(!strcmp(atts[i], "id1"))
					message->id1 = atts[i+1];
				else if(!strcmp(atts[i], "id2"))
					message->id2 = atts[i+1];
				else {
					message->error(428, atts[i]);
					return;
				}
				i += 2;
			}
			if(message->pkg->callback->getConnection(message->pkg, message->id1) == NULL) {
				message->error(((message->id1.find("~") != string::npos) ? 412 : 406), message->id1 + " does not exist");
				return;
			}
			if(message->pkg->callback->getConnection(message->pkg, message->id2) == NULL) {
				message->error(((message->id2.find("~") != string::npos) ? 412 : 406), message->id2 + " does not exist");
				return;
			}
		} else if(message->childs.back() == "unjoin") {
			if(!atts) {	// id1 and id2 were not specified
				message->error(400, "id1, id2");
				return;
			}
			int i = 0;
			while(atts[i]) {
				if(!atts[i])
					break;
				if(!strcmp(atts[i], "id1"))
					message->id1 = atts[i+1];
				else if(!strcmp(atts[i], "id2"))
					message->id2 = atts[i+1];
				else {
					message->error(428, atts[i]);
					return;
				}
				i += 2;
			}
			if(message->pkg->callback->getConnection(message->pkg, message->id1) == NULL) {
				message->error(((message->id1.find("~") != string::npos) ? 412 : 406), message->id1 + " does not exist");
				return;
			}
			if(message->pkg->callback->getConnection(message->pkg, message->id2) == NULL) {
				message->error(((message->id2.find("~") != string::npos) ? 412 : 406), message->id2 + " does not exist");
				return;
			}
		} else if(message->childs.back() == "audit") {
			string confId = "";
			bool capabilities = true, mixers = true;
			if(atts) {
				int i = 0;
				while(atts[i]) {	// We don't care about other attributes
					if(!atts[i])
						break;
					if(!strcmp(atts[i], "conferenceid"))
						confId = atts[i+1];
					else if(!strcmp(atts[i], "capabilities")) {
						bool ok;
						capabilities = booleanValue(atts[i+1], &ok);
						if(!ok) {
							message->error(400, "capabilities");
							return;
						}
					} else if(!strcmp(atts[i], "mixers")) {
						bool ok;
						mixers = booleanValue(atts[i+1], &ok);
						if(!ok) {
							message->error(400, "mixers");
							return;
						}
					} else {
						message->error(428, atts[i]);
						return;
					}
					i += 2;
				}
				if((confId != "") && (message->pkg->confs[confId] == NULL)) {
					message->error(406, confId);
					return;
				}
			}
			message->auditCapabilities = capabilities;
			message->auditMixers = mixers;
			message->auditConference = confId;
		} else {
			message->error(428, name);
			return;
		}
	} else if(message->level == 3) {
		// Handle possible elements at level 3 according to level 2
		message->childs.pop_back();
		string father = message->childs.back();
		message->childs.push_back(name);
		if(message->childs.back() == "audio-mixing") {
			if((father != "createconference") &&
				(father != "modifyconference"))
					return;
			int mixingtype = MIXING_NONE;
			uint16_t n = 0;
			if(!atts) {	// mixing type was not specified
				mixingtype = MIXING_NBEST;
			} else {
				int i = 0;
				while(atts[i]) {
					if(!atts[i])
						break;
					if(!strcmp(atts[i], "type")) {
						if(mixingtype != MIXING_NONE) {
							message->error(400, "audio-mixing type already set");	// FIXME
							return;
						}
						if(!strcmp(atts[i+1], "nbest"))
							mixingtype = MIXING_NBEST;
						else if(!strcmp(atts[i+1], "controller"))
							mixingtype = MIXING_CONTROLLER;
						else {
								message->error(421, atts[i+1]);	// FIXME 421 or 400?
								return;
						}
					} else if(!strcmp(atts[i], "n")) {
						bool ok = false;
						n = positiveInteger(atts[i+1], &ok);
						if(!ok) {		// Invalid value for attribute
							message->error(400, "n");
							return;
						}	
					}
					i += 2;
				}
			}
			if(mixingtype == MIXING_NONE)
				mixingtype = MIXING_NBEST;
			if(message->conf)
				message->conf->setAudioMixingType(mixingtype, n);
		} else if(message->childs.back() == "codecs") {
			if((father != "createconference") &&
				(father != "modifyconference"))
					return;
			// TODO
		} else if(message->childs.back() == "video-layouts") {
		 	message->error(423, name);
			return;
		} else if(message->childs.back() == "video-switch") {
		 	message->error(424, name);
			return;
		} else if(message->childs.back() == "subscribe") {
			if((father != "createconference") &&
				(father != "modifyconference"))
					return;
			// This element has no attributes
		} else if(message->childs.back() == "stream") {
			if((father != "join") &&
				(father != "modifyjoin") &&
				(father != "unjoin"))
					return;
			if(!atts) {	// media was not specified
				message->error(400, "media");
				return;
			}
			MixerStream *stream = new MixerStream();
			int i = 0;
			bool mediafound = false;
			while(atts[i]) {
				if(!atts[i])
					break;
				if(!strcmp(atts[i], "media")) {
					mediafound = true;
					stream->media = atts[i+1];
				} else if(!strcmp(atts[i], "label")) {
					stream->label = atts[i+1];
				} else if(!strcmp(atts[i], "direction")) {
					string direction = atts[i+1];
					if(direction == "sendrecv")
						stream->direction = SENDRECV;
					else if(direction == "sendonly")
						stream->direction = SENDONLY;
					else if(direction == "recvonly")
						stream->direction = RECVONLY;
					else if(direction == "inactive")
						stream->direction = INACTIVE;
					else {
						delete stream;
						stringstream error;
						error << "Unsupported direction '" << atts[i+1] << "'";
						message->error(407, error.str());
						return;
					}
				} else {
					delete stream;
					message->error(428, atts[i]);
					return;
				}
				i += 2;
			}
			if(!mediafound) {	// media was not specified
				delete stream;
				message->error(400, "media");
				return;
			}
			if(stream->media == "audio")
				stream->mediaType = MEDIACTRL_MEDIA_AUDIO;
			else {
				message->error(407, "Invalid media '" + stream->media + "'");
				delete stream;
				return;
			}
			// FIXME We check the validity of the label only at the end of parsing, since it's there that we enforce the request...
			message->streams.push_back(stream);
		} else {
		 	message->error(428, name);
			return;
		}
	} else if(message->level == 4) {
		// Handle possible elements at level 4 according to level 3
		message->childs.pop_back();
		if(message->childs.back() == "stream") {
			message->childs.push_back(name);
			MixerStream *stream = message->streams.back();
			string volume_type = "", volume_value = "";
			if(message->childs.back() == "volume") {
				if(!atts)	// no attributes were specified
					return;
				int i = 0;
				while(atts[i]) {
					if(!atts[i])
						break;
					if(!strcmp(atts[i], "controltype"))
						volume_type = atts[i+1];
					else if(!strcmp(atts[i], "value"))
						volume_value = atts[i+1];
					i += 2;
				}
				// Evaluate <volume>
				if(volume_type == "automatic") {
					// TODO automatic=??
				} else if(volume_type == "setgain") {
					// TODO Check if this is syntactically correct
					stream->volume = getPercentVolume(atoi(volume_value.c_str()));
				} else if(volume_type == "setstate") {
					if(volume_value == "mute")
						stream->volume = VOLUME_MUTE;
					else if(volume_value == "unmute")
						stream->volume = VOLUME_UNMUTE;	// FIXME should be previous value...
					else
						message->error(400, "value");
				} else
					message->error(400, "controltype");
				return;
			} else if(message->childs.back() == "clamp") {
				if(!atts)	// tones were not specified
					return;
				if(!strcmp(atts[0], "tones"))
					stream->clamp_tones = atts[1];
				else
					message->error(428, atts[0]);
				return;
			} else if(message->childs.back() == "region") {
				// This element has no attributes
				return;
			} else if(message->childs.back() == "priority") {
				// This element has no attributes
				return;
			} else {
				message->error(428, name);
				return;
			}
			if(stream->clamp_tones == "")	// Mandatory
				message->error(400, "clamp/tones");
		} else if(message->childs.back() == "subscribe") {
			if(message->conf == NULL)
				return;
			message->childs.push_back(name);
			if(message->childs.back() == "active-talkers-sub") {
				uint32_t interval = 0;
				if(atts) {
					if(!strcmp(atts[0], "interval")) {
						bool ok = false;
						interval = positiveInteger(atts[1], &ok);
						if(!ok) {
							message->error(400, "active-talkers-sub");
							return;
						}
					} else {
						message->error(428, atts[0]);
						return;
					}
				}
				message->conf->subscribe(SUBSCRIBE_ACTIVETALKERS, interval);
			} else {
				message->error(428, name);
				return;
			}
		} else if(message->childs.back() == "video-layouts") {
		 	message->error(423, name);
			return;
		} else {
			message->error(428, name);
			return;
		}
		return;
	} else {	// Every deeper element is an unknown one
		message->error(428, name);
		return;
	}
}

static void XMLCALL valueElement(void *msg, const XML_Char *s, int len)
{
	MixerMessage *message = (MixerMessage *)msg;
	if(message->scanonly)
		return;
	if(message->stop)
		return;
	char value[len+1];
	int i=0;
	int pos = 0;
	while(i < len) {	// FIXME...
		if((s[i] == 0x09) || (s[i] == 0x0a) || (s[i] == 0x0d)) {
			i++;
			continue;
		} else if(s[i] == 0x20) {
			if(pos == 0) {	// Only skip backspaces *before* the content itself
				i++;
				continue;
			}
		}
		memcpy(value+pos, s+i, 1);
		i++;
		pos++;
	}
	if(pos == 0)
		return;
	// Now remove backspaces at the end
	i = pos;
	while(1) {
		i--;
		if(value[i] != 0x20);
			break;
		pos = i;
	}
	value[pos] = '\0';
	cout << "Ignoring '" << value << "'" << endl;
}

static void XMLCALL endElement(void *msg, const char *name)
{
	MixerMessage *message = (MixerMessage *)msg;
	if(message->scanonly)
		return;
	if(message->stop)	// Means that something was wrong...
		return;
	message->level--;
	message->childs.pop_back();
	if(message->level == 0) {	// We have come back to the root
		cout << "[MIXER] Completed parsing of the request: " << message->request << endl;
		if(message->request == "audit") {	// An auditing request, no manipulation of mixers is involved
			stringstream blob;
			blob //<< "<?xml version=\"1.0\"?>"
				<< "<mscmixer version=\"1.0\" xmlns=\"urn:ietf:params:xml:ns:msc-mixer\">"
				<< "<auditresponse status=\"200\">";
			if(message->auditCapabilities) {
				// Capabilities
				blob << "<capabilities>"
				//	Codecs	(TODO ask the core)
					<< "<codecs>"
					<< "<codec><subtype>telephony-event</subtype></codec>"
					<< "<codec><subtype>PCMA</subtype></codec>"
					<< "<codec><subtype>PCMU</subtype></codec>"
					<< "<codec><subtype>GSM</subtype></codec>"
					<< "<codec><subtype>H.261</subtype></codec>"
					<< "<codec><subtype>H.263</subtype></codec>"
					<< "<codec><subtype>H.263+</subtype></codec>"
					<< "<codec><subtype>H.264</subtype></codec>"
					<< "</codecs>"
					<< "</capabilities>";
			}
			if(message->auditMixers) {
				// Dialogs
				if(message->pkg->confs.empty())
					blob << "<mixers/>";
				else {
					
					blob << "<mixers>";
					// Conferences
					map<string, MixerConference *>::iterator iter;
					MixerConference *conf = NULL;
					for(iter = message->pkg->confs.begin(); iter != message->pkg->confs.end(); iter++) {
						conf = iter->second;
						if(conf == NULL)
							continue;
						if((message->auditConference != "") && (message->auditConference != iter->first))
							continue;	// Not the conference we're interested to
						if(!conf->checkSender(message->requester)) {	// This mixer was not created by this requester
							if(message->auditConference == "")
								continue;	// It's ok, we just skip this mixer...
							// If we get here, it means an unauthorized requester explicitly wanted info on this mixer
							message->pkg->callback->report(message->pkg, message->requester, message->tid, 403, 0);			// FIXME
							message->stop = true;
							return;
						}
						blob << "<conferenceaudit conferenceid=\"" << iter->first << "\">";
						// Participants
						if(conf->nodes.empty())
							blob << "<participants/>";
						else {
							blob << "<participants>";
							map<MixerNode *, int>::iterator iter2;
							for(iter2 = conf->nodes.begin(); iter2 != conf->nodes.end(); iter2++) {
								if(iter2->first == NULL)
									continue;
								blob << "<participant id=\"" << iter2->first->getConId() << "\"/>";
							}
							blob << "</participants>";
						}
						blob << "</conferenceaudit>";
					}
					map<ControlPackageConnection *, MixerNode *>::iterator iter1;
					map<MixerNode *, int>::iterator iter2;
					MixerNode *node = NULL;
					for(iter1 = message->pkg->nodes.begin(); iter1 != message->pkg->nodes.end(); iter1++) {
						node = iter1->second;
						if(node == NULL)
							continue;
						if((message->auditConference != "") && (message->auditConference != node->getConId()))
							continue;	// Not the conference we're interested to
						if(node->getConnection() == NULL)	
							continue;	// Invalid node?
						if(node->getConnection()->getType() == CPC_CONFERENCE) {
							MixerConference *conf = (MixerConference*)node;
							if(!conf->checkSender(message->requester))	// This mixer was not created by this requester
								continue;
						}
						// TODO Should we enforce security considerations on joinaudit as well?
						if(!node->nodes.empty()) {
							for(iter2 = node->nodes.begin(); iter2 != node->nodes.end(); iter2++) {
								if(iter2->first == NULL)
									continue;
								if(!node->checkSender(message->requester))	// This mixer was not created by this requester
									continue;
								if((message->auditConference != "") && (message->auditConference == node->getConId()))
									blob << "<joinaudit id1=\"" << node->getConId() << "\" id2=\"" << iter2->first->getConId() << "\"/>";
								else if(node->getConId() <= iter2->first->getConId())	// To avoid duplicates
									blob << "<joinaudit id1=\"" << node->getConId() << "\" id2=\"" << iter2->first->getConId() << "\"/>";
							}
						}
						// TODO add information to joinaudit
						// blob << "</joinaudit>";
					}
					// Joins
					
					blob << "</mixers>";
				}
			}
			// We're done
			blob << "</auditresponse></mscmixer>";
			message->pkg->callback->report(message->pkg, message->requester, message->tid, 200, 10, blob.str());
		} else {
			if(message->newconf && message->conf) {
				if(message->conf->getAudioMixingType() == MIXING_NONE) {
					// audio-mixing is mandatory, handle error!
					message->error(421, "audio-mixing");
					delete message->conf;
				} else {
					cout << "[MIXER]     Created new conference:    " << message->conf->getConId() << endl;
					cout << "[MIXER]         Reserved Talkers:      " << dec << message->conf->getReservedTalkers() << endl;
					cout << "[MIXER]         Reserved Listeners:    " << dec << message->conf->getReservedListeners() << endl;
					cout << "[MIXER]         Audio Mixing-type:     ";
					if(message->conf->getAudioMixingType() == MIXING_NBEST)
						cout << "n-best (N=" << dec << message->conf->getAudioMixingN() << ")" << endl;
					else if(message->conf->getAudioMixingType() == MIXING_CONTROLLER)
						cout << "controller" << endl;
					else
						cout << "???" << endl;
					// Actually start the conference
					message->conf->start();
					stringstream blob;
					blob //<< "<?xml version=\"1.0\"?>"
						<< "<mscmixer version=\"1.0\" xmlns=\"urn:ietf:params:xml:ns:msc-mixer\">"
						<< "<response status=\"200\" reason=\"Conference created\" conferenceid=\"" << message->conf->getConId() << "\"/>"
						<< "</mscmixer>";
					message->pkg->callback->report(message->pkg, message->requester, message->tid, 200, 10, blob.str());	// FIXME
				}
			}
			if(message->request == "modifyconference") {
				// TODO We do nothing in here currently... but we report a success anyway
				stringstream blob;
				blob //<< "<?xml version=\"1.0\"?>"
					<< "<mscmixer version=\"1.0\" xmlns=\"urn:ietf:params:xml:ns:msc-mixer\">"
					<< "<response status=\"200\" reason=\"Conference modified\" conferenceid=\"" << message->conf->getConId() << "\"/>"
					<< "</mscmixer>";
				message->pkg->callback->report(message->pkg, message->requester, message->tid, 200, 10, blob.str());
			} else if(message->request == "destroyconference") {
				// Destroy the conference
				string confId = message->conf->getConId();
				delete message->conf;
				stringstream blob;
				blob //<< "<?xml version=\"1.0\"?>"
					<< "<mscmixer version=\"1.0\" xmlns=\"urn:ietf:params:xml:ns:msc-mixer\">"
					<< "<response status=\"200\" reason=\"Conference removed\" conferenceid=\"" << confId << "\"/>"
					<< "</mscmixer>";
				message->pkg->callback->report(message->pkg, message->requester, message->tid, 200, 10, blob.str());
			} else if(message->request == "join") {
				message->parseStreams();
				if(message->stop) {
					cout << "[MIXER] The parsing of the 'join' request failed" << endl;
				} else {
					// TODO Actually join according to the provided settings
					bool audioSuccess = false;
					// Audio
					if((message->audioNode[0] == NULL) && (message->audioNode[1] == NULL))
						audioSuccess = true;	// Audio was not requested to be joined, but we mark it as a success anyway
					if((message->audioNode[0] != NULL) && (message->audioNode[1] != NULL)) {
						// There's an audio link to setup between the two connections
						MixerNode *node1 = message->pkg->nodes[message->audioNode[0]];
						if(node1 == NULL) {	// There's not a valid MixerNode yet for this connection, create one now
							MixerConnection *nodeCon1 = new MixerConnection();
							nodeCon1->setup(message->pkg, message->requester, message->audioNode[0], message->audioNode[0]);	// FIXME Involve masterConnection
							node1 = (MixerNode*)nodeCon1;
							message->pkg->nodes[message->audioNode[0]] = node1;
						}
						if(!node1->checkSender(message->requester)) {	// Unauthorized: not the original requester
							message->pkg->callback->report(message->pkg, message->requester, message->tid, 403, 0);			// FIXME
							message->stop = true;
							return;
						}
						MixerNode *node2 = message->pkg->nodes[message->audioNode[1]];
						if(node2 == NULL) {	// There's not a valid MixerNode yet for this connection, create one now
							MixerConnection *nodeCon2 = new MixerConnection();
							nodeCon2->setup(message->pkg, message->requester, message->audioNode[1], message->audioNode[1]);	// FIXME Involve masterConnection
							node2 = (MixerNode*)nodeCon2;
							message->pkg->nodes[message->audioNode[1]] = node2;
						}
						if(!node2->checkSender(message->requester)) {	// Unauthorized: not the original requester
							message->pkg->callback->report(message->pkg, message->requester, message->tid, 403, 0);			// FIXME
							message->stop = true;
							return;
						}
						int err = 0;
						if(node1->check(node2))
							message->error(408, "audio error (id1)");
						else {
							err = node1->attach(node2, message->audioDirection, message->audioVolume[0]);
							if(err < 0) {
								if(err == -2)	// Already joined
									message->error(408, "audio error (id1)");
								else
									message->error(411, "audio error (id1)");	// FIXME 411 or 426?
							} else {
								if(node1 == node2) {	// If this is an echo test, we already succeeded
									audioSuccess = true;
								} else {
									if(node2->check(node1))
										message->error(408, "audio error (id2)");
									else {
										err = node2->attach(node1, getReverseDirection(message->audioDirection), message->audioVolume[1]);
										if(err < 0) {
											if(err == -2)	// Already joined
												message->error(408, "audio error (id2)");
											else {
												node1->detach(node2);
												message->error(411, "audio error (id2)");
												message->pkg->notifyUnjoin(message->requester, node1->getConId(), node2->getConId(), UNJOIN_ERROR);
											}
										} else
											audioSuccess = true;
									}
								}
							}
						}
					}
					if(!message->stop) {
						if(!audioSuccess) {	// Audio could not be joined
							cout << "[MIXER] join UNsuccessful!! " << message->id1 << "/" << message->id2 << endl;
							if(!message->stop)
								message->error(411, "nothing to join");
						} else if(!message->stop) {
							cout << "[MIXER] join successful: " << message->id1 << "/" << message->id2 << endl;
							// Create a dummy string to avoid duplicate unjoin-notify events
							string joinedNodes = message->id1 + "~" + message->id2;
							message->pkg->joins.push_back(joinedNodes);
							message->pkg->joins.sort();
							message->pkg->joins.unique();
							stringstream blob;
							blob << "<mscmixer version=\"1.0\" xmlns=\"urn:ietf:params:xml:ns:msc-mixer\">"
							//	<< "<?xml version=\"1.0\"?>"
								<< "<response status=\"200\" reason=\"Join successful\"/>"
								<< "</mscmixer>";
							message->pkg->callback->report(message->pkg, message->requester, message->tid, 200, 10, blob.str());
						}
					}
				}
			} else if(message->request == "modifyjoin") {	// We ignore the <stream> element for now
				message->parseStreams();
				if(message->stop) {
					cout << "[MIXER] The parsing of the 'modifyjoin' request failed" << endl;
				} else {
					// TODO Actually modify the join according to the provided settings
					bool audioSuccess = false;
					// Audio
					if((message->audioNode[0] == NULL) && (message->audioNode[1] == NULL))
						audioSuccess = true;	// Audio was not requested to be modified, but we mark it as a success anyway
					else if((message->audioNode[0] != NULL) && (message->audioNode[1] != NULL)) {
						// There's an audio link to setup between the two connections
						MixerNode *node1 = message->pkg->nodes[message->audioNode[0]];
						if(node1 == NULL)	// There's not a valid MixerNode for this connection
							message->error(426, "audio error (id1)");
						else {
							if(!node1->checkSender(message->requester)) {	// Unauthorized: not the original requester
								message->pkg->callback->report(message->pkg, message->requester, message->tid, 403, 0);			// FIXME
								message->stop = true;
								return;
							}
							MixerNode *node2 = message->pkg->nodes[message->audioNode[1]];
							if(node2 == NULL)	// There's not a valid MixerNode for this connection
								message->error(426, "audio error (id2)");
							else {
								if(!node2->checkSender(message->requester)) {	// Unauthorized: not the original requester
									message->pkg->callback->report(message->pkg, message->requester, message->tid, 403, 0);			// FIXME
									message->stop = true;
									return;
								}
								int err = 0;
								if(!node1->check(node2))
									message->error(409, "audio error (id1)");
								else {
									err = node1->modify(node2, message->audioDirection, message->audioVolume[0]);
									if(err < 0) {
										if(err == -2)	// Not joined
											message->error(409, "audio error (id1)");
										else
											message->error(411, "audio error (id1)");	// FIXME 411 or 426?
									} else {
										if(node1 == node2) {	// If this is an echo test, we already succeeded
											audioSuccess = true;
										} else {
											if(!node2->check(node1))
												message->error(409, "audio error (id2)");
											else {
												err = node2->modify(node1, getReverseDirection(message->audioDirection), message->audioVolume[1]);
												if(err < 0) {
													// TODO We should reverse the last change...
													if(err == -2)	// Not joined
														message->error(409, "audio error (id2)");
													else
														message->error(411, "audio error (id2)");	// FIXME 411 or 426?
												} else
													audioSuccess = true;
											}
										}
									}
								}
							}
						}
					}
					if(!message->stop) {
						if(!audioSuccess) {	// Audio could not be modified
							cout << "[MIXER] modifyjoin UNsuccessful!! " << message->id1 << "/" << message->id2 << endl;
							if(!message->stop)
								message->error(411, "nothing to modify");
						} else if(!message->stop) {
							cout << "[MIXER] modifyjoin successful: " << message->id1 << "/" << message->id2 << endl;
							stringstream blob;
							blob << "<mscmixer version=\"1.0\" xmlns=\"urn:ietf:params:xml:ns:msc-mixer\">"
							//	<< "<?xml version=\"1.0\"?>"
								<< "<response status=\"200\" reason=\"Join modified\"/>"
								<< "</mscmixer>";
							message->pkg->callback->report(message->pkg, message->requester, message->tid, 200, 10, blob.str());
						}
					}
				}
			} else if(message->request == "unjoin") {	// We ignore the <stream> element for now
				message->parseStreams();
				if(message->stop) {
					cout << "[MIXER] The parsing of the 'unjoin' request failed" << endl;
				} else {
					// TODO Actually remove the join according to the provided settings
					bool audioSuccess = false;
					// Audio
					if((message->audioNode[0] == NULL) && (message->audioNode[1] == NULL))
						audioSuccess = true;	// Audio was not requested to be unjoined, but we mark it as a success anyway
					if((message->audioNode[0] != NULL) && (message->audioNode[1] != NULL)) {
						// There's an audio link to setup between the two connections
						MixerNode *node1 = message->pkg->nodes[message->audioNode[0]];
						if(node1 == NULL)	// There's not a valid MixerNode for this connection
							message->error(426, "audio error (id1)");
						else {
							if(!node1->checkSender(message->requester)) {	// Unauthorized: not the original requester
								message->pkg->callback->report(message->pkg, message->requester, message->tid, 403, 0);			// FIXME
								message->stop = true;
								return;
							}
							MixerNode *node2 = message->pkg->nodes[message->audioNode[1]];
							if(node2 == NULL)	// There's not a valid MixerNode for this connection
								message->error(426, "audio error (id2)");
							else {
								if(!node2->checkSender(message->requester)) {	// Unauthorized: not the original requester
									message->pkg->callback->report(message->pkg, message->requester, message->tid, 403, 0);			// FIXME
									message->stop = true;
									return;
								}
								int err = 0;
								if(!node1->check(node2))
									message->error(409, "audio error (id1)");
								else {
									err = node1->detach(node2);
									if(err < 0) {
										if(err == -2)	// Already unjoined
											message->error(409, "audio error (id1)");
										else
											message->error(411, "audio error (id1)");	// FIXME 411 or 426?
									} else {
										if(node1 == node2) {	// If this is an echo test, we already succeeded
											message->pkg->notifyUnjoin(message->requester, node1->getConId(), node2->getConId(), UNJOIN_REQUEST);
											audioSuccess = true;
										} else {
											if(!node2->check(node1))
												message->error(409, "audio error (id2)");
											else {
												err = node2->detach(node1);
												if(err < 0) {
													// TODO We should reverse the last change...
													if(err == -2)	// Already unjoined
														message->error(409, "audio error (id2)");
													else
														message->error(411, "audio error (id2)");	// FIXME 411 or 426?
												} else {
													message->pkg->notifyUnjoin(message->requester, node1->getConId(), node2->getConId(), UNJOIN_REQUEST);
													audioSuccess = true;
												}
											}
										}
									}
								}
							}
						}
					}
					if(!message->stop) {
						if(!audioSuccess) {	// Audio could not be unlinked
							cout << "[MIXER] unjoin UNsuccessful!! " << message->id1 << "/" << message->id2 << endl;
							if(!message->stop)
								message->error(411, "nothing to unjoin");
						} else if(!message->stop) {
							cout << "[MIXER] unjoin successful: " << message->id1 << "/" << message->id2 << endl;
							stringstream blob;
							blob << "<mscmixer version=\"1.0\" xmlns=\"urn:ietf:params:xml:ns:msc-mixer\">"
							//	<< "<?xml version=\"1.0\"?>"
								<< "<response status=\"200\" reason=\"Join removed\"/>"
								<< "</mscmixer>";
							message->pkg->callback->report(message->pkg, message->requester, message->tid, 200, 10, blob.str());
						}
					}
				}
			}
		}
	}
}
