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

#ifndef _MEDIA_CTRL_RTP_H
#define _MEDIA_CTRL_RTP_H

/*! \file
 *
 * \brief Real-time Transport Protocol (RTP) Transactions Handler
 *
 * \author Lorenzo Miniero <lorenzo.miniero@unina.it>
 *
 * \ingroup core
 * \ref core
 */

#include <map>
#include <cc++/config.h>
#include <cc++/socket.h>

// oRTP
#include "ortp/ortp.h"
#include <ortp/telephonyevents.h>

#include "MediaCtrlCodec.h"

#include "MediaCtrlMemory.h"


/// Static initializer for oRTP related stuff
extern void rtpSetup(void);
/// Static method to cleanup all oRTP related stuff
extern void rtpCleanup(void);


using namespace std;
using namespace ost;


namespace mediactrl {

/// Available media types
enum rtp_media_types {
	/*! audio */
	MEDIACTRL_AUDIO = 0,
};

/// Available media directions
enum rtp_media_directions {
	/*! Can send and receive */
	MEDIACTRL_SENDRECV = 0,
	/*! Can only send */
	MEDIACTRL_SENDONLY,
	/*! Can only receive */
	MEDIACTRL_RECVONLY,
	/*! Can neither send nor receive */
	MEDIACTRL_INACTIVE,
};


class MediaCtrlRtpChannel;
/// List of MediaCtrlRtpChannel instances
typedef list<MediaCtrlRtpChannel *> MediaCtrlRtpChannels;

/// RTP events listener
/**
* @class MediaCtrlRtpManager MediaCtrlRtp.h
* An abstract class implemented by the interested objects (usually the MediaCtrlSipTransaction instance owning one or more MediaCtrlRtpChannel objects) in order to receive callback notifications related to RTP events.
*/
class MediaCtrlRtpManager {
	public:
		MediaCtrlRtpManager() {};
		virtual ~MediaCtrlRtpManager() {};

		virtual MediaCtrlCodec *createCodec(int codec) = 0;
		virtual int getBlockLen(int codec) = 0;

		virtual void payloadTypeChanged(MediaCtrlRtpChannel *rtpChannel, int pt) = 0;
		virtual void incomingFrame(MediaCtrlRtpChannel *rtpChannel, MediaCtrlFrame *frame) = 0;
		virtual void incomingDtmf(MediaCtrlRtpChannel *rtpChannel, int type) = 0;
		virtual void frameSent(MediaCtrlRtpChannel *rtpChannel, MediaCtrlFrame *frame) = 0;
		virtual void channelLocked(MediaCtrlRtpChannel *rtpChannel) = 0;
		virtual void channelUnlocked(MediaCtrlRtpChannel *rtpChannel) = 0;
		virtual void channelClosed(string label) = 0;
};

/// RTP Transaction class
/**
* @class MediaCtrlRtpChannel MediaCtrlRtp.h
* The class implementing RTP channels and their management.
*/
class MediaCtrlRtpChannel : public gc, public Thread {
	public:
		/**
		* @fn MediaCtrlRtpChannel(const InetHostAddress &ia, int media)
		* Constructor. The address is purely informational. The port is chosen randomly.
		* @param ia The local address (IP)
		* @param media The media type (MEDIACTRL_MEDIA_AUDIO)
		*/
		MediaCtrlRtpChannel(const InetHostAddress &ia, int media=MEDIACTRL_MEDIA_AUDIO);
		/**
		* @fn ~MediaCtrlRtpChannel()
		* Destructor. Also shuts down the RTP connection..
		*/
		~MediaCtrlRtpChannel();

		/**
		* @fn setPeer(const InetHostAddress &ia, uint16_t dataPort)
		* Sets the RTP peer (to whom this channel is attached to).
		* @param ia The IP of the RTP peer
		* @param dataPort The port of the RTP peer
		* @returns true on success, false otherwise
		*/
		bool setPeer(const InetHostAddress &ia, uint16_t dataPort);
		/**
		* @fn setPayloadType(int pt)
		* Specifies the payload type of the media that will flow on the channel.
		* @param pt The payload type
		* @note In case the payload type changes, this method is triggered internally to reflect the change
		*/
		void setPayloadType(int pt);
		/**
		* @fn setDirection(int direction)
		* Sets the allowed direction for the media flowing on the channel.
		* @param direction One of the media_direction enumerated values
		* @returns true on success, false otherwise
		*/
		bool setDirection(int direction);
		/**
		* @fn addSetting(string value)
		* Adds a specific setting for the codec handling the media, if supported.
		* @param value The value(s) for the setting as extracted from an SDP fmtp attribute
		* @returns The fmtp attribute as a string that can be placed as answer in the SDP
		* @note This method is to be invoked BEFORE the codec is actually started, otherwise it is not effective
		*/
		string addSetting(string value);
		/**
		* @fn setClockRate(int clockrate)
		* Sets the timestamp increase for outgoing packets, since it differs for audio and video.
		* @param clockrate The timestamp increase related to the clockrate (e.g. 160 for audio)
		*/
		void setClockRate(int clockrate);
		/**
		* @fn getMediaType()
		* Gets the type (audio/video) of the media flowing on the channel.
		* @returns The media type
		*/
		int getMediaType() { return media; };
		/**
		* @fn getPayloadType()
		* Gets the payload type of the media flowing on the channel.
		* @returns The payload type
		*/
		int getPayloadType() { return pt; };
		/**
		* @fn getDirection()
		* Gets the currently allowed direction for media flowing on the channel.
		* @returns The direction value as defined in the media_directions enumeration
		*/
		int getDirection() { return direction; };
		/**
		* @fn getClockRate()
		* Gets the timestamp increase when getting packets, since it differs for audio and video.
		* @returns The timestamp increase related to the clockrate (e.g. 160 for audio)
		*/
		int getClockRate() { return clockrate; };
		/**
		* @fn getFlags()
		* Gets the flags mask associated with the encoding of the media flowing on the channel.
		* @returns The flags mask
		*/
		uint32_t getFlags() { return flags; };

		/**
		* @fn getLabel()
		* Gets the label assigned to and associated with the media.
		* @returns The label as a string
		*/
		string getLabel() { return label; };

		/**
		* @fn getSrcIp()
		* Gets the source (local) IP
		* @returns The source (local) IP as a string
		*/
		string getSrcIp() { return srcIp.getHostname(); };
		/**
		* @fn getSrcPort()
		* Gets the source (local) port
		* @returns The source (local) port
		*/
		uint16_t getSrcPort() { return srcPort; };
		/**
		* @fn getDstIp()
		* Gets the destination (remote) IP
		* @returns The destination (remote) IP as a string
		*/
		string getDstIp() { return dstIp.getHostname(); };
		/**
		* @fn getDstPort()
		* Gets the destination (remote) port
		* @returns The destination (remote) port
		*/
		uint16_t getDstPort() { return dstPort; };

		/**
		* @fn lock(void *owner)
		* Locks the channel, meaning that all subsequent requests to send a frame on the channel are rejected.
		* @param owner Opaque pointer identifying the "locker"
		* @note This method also triggers the 'channelLocked' event. This locking mechanism is mainly used for announcements, since they are queued as a complete list of frames on the channel in one call (where the first frame in the list is a locking frame, and the last an unlocking frame), leaving the correct RTP tempification to the MediaCtrlRtpChannel instance
		*/
		void lock(void *owner);
		/**
		* @fn unlock(void *owner)
		* Unlocks the channel, meaning that all subsequent requests to send a frame on the channel can be accepted again.
		* @param owner Opaque pointer identifying the "unlocker"
		* @note This method also triggers the 'channelUnlocked' event. This locking mechanism is mainly used for announcements, since they are queued as a complete list of frames on the channel in one call (where the first frame in the list is a locking frame, and the last an unlocking frame), leaving the correct RTP tempification to the MediaCtrlRtpChannel instance
		*/
		void unlock(void *owner);

		void clearFrames(MediaCtrlFrames *frameslist);
		/**
		* @fn clearDtmfBuffer()
		* Clears all the digits that may have been buffered so far from this channel.
		*/
		void clearDtmfBuffer();
		/**
		* @fn getNextDtmfBuffer()
		* Gets the first digit in the DTMF digits queue, also removing it from the queue.
		* @returns The tone as defined in the enumerator, if available, -1 otherwise
		*/
		int getNextDtmfBuffer();
		/**
		* @fn incomingData(uint8_t *buffer, int len, bool last)
		* This callback triggers the channel about new incoming data, i.e. a received frame.
		* @param buffer The buffer containing the frame data
		* @param len The length in bytes of the buffer
		* @param last Whether this data completes previously passed stuff (matches the RTP MarkerBit)
		* @note This callback is only meaningful to the instance itself: in fact, it is triggered whenever the global, hidden, MediaCtrlRtpSet instance receives new data on this channel. The channel instance, then, builds a MediaCtrlFrame instance out of the data (if the data is complete, otherwise it buffers it to build the frame subsequently), and triggers the incomingFrame event to pass it to the interested listeners
		*/
		void incomingData(uint8_t *buffer, int len, bool last=true);
		/**
		* @fn sendFrame(MediaCtrlFrame *frame)
		* This method sends a frame to the RTP peer, encoding it if necessary.
		* @param frame The frame to queue
		*/
		void sendFrame(MediaCtrlFrame *frame);

		/**
		* @fn setManager(MediaCtrlRtpManager *manager)
		* Sets the callback to notify our owner (the SIP session this channel belongs to)
		* @param manager The listener
		* @note All the events are sent to a single listener, the SIP session (a MediaCtrlSipTransaction instance) this channel belongs to: it is then the SIP session which spreads the event to all the related enpoints, and from there to the associated connections and packages
		*/
		void setManager(MediaCtrlRtpManager *manager) { this->rtpManager = manager; };
		/**
		* @fn incomingFrame(MediaCtrlFrame *frame)
		* This callback is triggered when a frame is received by the peer.
		* @param frame The incoming frame
		* @note This should never be called directly, since it is only used internally. It causes the same event to be notified to the specified listener.
		*/
		void incomingFrame(MediaCtrlFrame *frame);
		/**
		* @fn incomingDtmf(int type)
		* This callback is triggered when a DTMF tone is received by the peer.
		* @param type The incoming tone as defined in the enumerator
		* @note This should never be called directly, since it is only used internally. It causes the same event to be notified to the specified listener.
		*/
		void incomingDtmf(int type);

		void wakeUp(bool doIt);

	private:
		/**
		* @fn run()
		* The thread handling the outgoing RTP frames on this channel. Frames are first queued with the public inferface methods, and then automatically sent, opportunely tempificated, here.
		* @note The incoming frames are handled by another, hidden, class which manages all the RTP channels together as a set
		*/
		void run();

		bool alive;				/*!< Whether this channel is active (in the sense of "up and running") or not */

		MediaCtrlRtpManager *rtpManager;	/*! The SIP transaction handling us */

		RtpSession *rtpSession;			/*!< oRTP Session */
		InetHostAddress srcIp;			/*!< The source (local) IP address */
		uint16_t srcPort;			/*!< The source (local) port */
		InetHostAddress dstIp;			/*!< The destination (remote) IP address */
		uint16_t dstPort;			/*!< The destination (remote) port */

		int media;		/*!< Media type (audio/video) */
		int pt;			/*!< AVT Profile and Payload Type */	// (FIXME)
		int direction;		/*!< Media direction */
		string label;		/*!< SDP Label */
		int clockrate;		/*!< Step increase when getting RTP timestamped packets */
		uint32_t timing;	/*!< The timing in ms for RTP outgoing frames */
		uint32_t flags;		/*!< MediaCtrlFrame flags, of interest when creating the codec */

		MediaCtrlCodec *codec;	/*!< The codec handling the incoming and outgoing frames (shared pointer) */

		uint32_t lastTs;	/*!< The last sent timestamp */
		uint32_t lastIncomingTs;
		uint32_t num;		/*!< The relative timestamp to put in outgoing packets */
		DtmfTones tones;		/*!< List of bufferized DTMF tones */
		ost::Mutex *mTones;			/*!< Mutex for the frames list */

		bool locked;		/*!< The channel might be locked, e.g. in announcements */
		void *lockOwner;	/*!< Opaque pointer to the entity who's locked the channel */

		list<uint8_t *> packets;	/*!< List of incoming RTP packets, needed when more packets make a single frame (e.g. for video) */
		list<int> packetLens;		/*!< Size of the packets */

		bool active;
		ost::Conditional *cond;
};

}

#endif
