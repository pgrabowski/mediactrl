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

#ifndef _MEDIA_CTRL_SIP_H
#define _MEDIA_CTRL_SIP_H

/*! \file
 *
 * \brief Headers: Session Initiation Protocol (SIP) Transactions Handler
 *
 * \author Lorenzo Miniero <lorenzo.miniero@unina.it>
 *
 * \ingroup core
 * \ref core
 */

// reSIProcate
#include "resip/dum/AppDialog.hxx"
#include "resip/dum/AppDialogSet.hxx"
#include "resip/dum/AppDialogSetFactory.hxx"
#include "resip/dum/ClientRegistration.hxx"
#include "resip/dum/DialogUsageManager.hxx"
#include "resip/dum/DialogId.hxx"
#include "resip/dum/DumThread.hxx"
#include "resip/dum/InviteSessionHandler.hxx"
#include "resip/dum/ClientInviteSession.hxx"
#include "resip/dum/ServerInviteSession.hxx"
#include "resip/dum/MasterProfile.hxx"
#include "resip/dum/ServerRegistration.hxx"
#include "resip/dum/RegistrationHandler.hxx"
#include "resip/dum/InMemoryRegistrationDatabase.hxx"
#include "resip/dum/SubscriptionHandler.hxx"
#include "resip/stack/StackThread.hxx"
#include "resip/stack/ExtensionHeader.hxx"
#include "resip/stack/Helper.hxx"
#include "resip/stack/SdpContents.hxx"
#include "resip/stack/SipMessage.hxx"
#include "resip/stack/Uri.hxx"
#include "resip/stack/SipStack.hxx"
#include "resip/stack/DeprecatedDialog.hxx"
#include "resip/stack/SipStack.hxx"
#include "rutil/Logger.hxx"
#include "rutil/ThreadIf.hxx"

#include "MediaCtrlRtp.h"

#include "MediaCtrlMemory.h"

using namespace resip;
using namespace std;

#define RESIPROCATE_SUBSYSTEM Subsystem::TEST


namespace mediactrl {

class MediaCtrlSipTransaction;

/// SIP and RTP events listener
/**
* @class MediaCtrlSipManager MediaCtrlSip.h
* An abstract class implemented by any interested object in order to receive callback notifications related to SIP (less) and RTP (more) events.
*/
class MediaCtrlSipManager {
	public:
		MediaCtrlSipManager() {};
		virtual ~MediaCtrlSipManager() {};

		virtual void payloadTypeChanged(MediaCtrlSipTransaction *sipTransaction, MediaCtrlRtpChannel *rtpChannel, int pt) = 0;
		virtual void incomingFrame(MediaCtrlSipTransaction *sipTransaction, MediaCtrlRtpChannel *rtpChannel, MediaCtrlFrame *frame) = 0;
		virtual void incomingDtmf(MediaCtrlSipTransaction *sipTransaction, MediaCtrlRtpChannel *rtpChannel, int type) = 0;
		virtual void frameSent(MediaCtrlSipTransaction *sipTransaction, MediaCtrlRtpChannel *rtpChannel, MediaCtrlFrame *frame) = 0;
		virtual void channelLocked(MediaCtrlSipTransaction *sipTransaction, MediaCtrlRtpChannel *rtpChannel) = 0;
		virtual void channelUnlocked(MediaCtrlSipTransaction *sipTransaction, MediaCtrlRtpChannel *rtpChannel) = 0;
		virtual void channelClosed(string connectionId, string label) = 0;
};
/// List of MediaCtrlSipManager instances, needed to handle the map of lists (control packages attached to a connection)
typedef list<MediaCtrlSipManager *> MediaCtrlSipManagers;


/// Codec manager (needed by MediaCtrlSipTransaction)
/**
* @class MediaCtrlCodecManager MediaCtrlSip.h
* An abstract class implemented by the MediaCtrl core object in order to allow SIP and RTP objects to access codec functionality.
*/
class MediaCtrlCodecManager {
	public:
		MediaCtrlCodecManager() {};
		virtual ~MediaCtrlCodecManager() {};

		virtual MediaCtrlCodec *createCodec(int codec) = 0;
		virtual int getBlockLen(int codec) = 0;
};


/// SIP Transaction class
/**
* @class MediaCtrlSipTransaction MediaCtrlSip.h
* The class implementing all SIP-related stuff and transactions.
* @note The SIP state machine behaviour is not handled here, but in the core: here all the info about transactions is managed, and the session (including RTP channels) created and handled. Considering all the SIP stuff is accomplished by means of the reSIProcate library, the needed handles (ServerInviteSessionHandle and InviteSessionHandle) are part of the cass as well
*/
class MediaCtrlSipTransaction : public gc, public MediaCtrlRtpManager {
	public:
		/**
		* @fn MediaCtrlSipTransaction(string callId, ServerInviteSessionHandle sis)
		* Constructor. Initializes a new, currently being negotiated, transaction handler, by taking into account the relevant objects (the SIP Call-ID and the underlying reSIProcate handle).
		* @param callId The SIP Call-ID
		* @param sis The reSIProcate server (since we receive INVITEs) session handle
		* @note Objects of this class are instanciated as soon as a new INVITE is received (assuming the Call-ID is different, of course...). This means that the instance is only subsequently enriched with information, as soon as the UA's offer is managed.
		*/
		MediaCtrlSipTransaction(string callId, ServerInviteSessionHandle sis);
		/**
		* @fn ~MediaCtrlSipTransaction()
		* Destructor.
		* @note The instance is destroyed only *after* the actual SIP transaction has been terminated. Destroying the instance does not cause the reSIProcate object to be destroyed as well.
		*/
		~MediaCtrlSipTransaction();

		void setAddress(InetHostAddress &address) { this->address = address; };
		void setAS(string cfwId) { this->as = true; this->cfwId = cfwId; };
		bool isAS() { return as; };
		void setInviteHandler(InviteSessionHandle is) { this->is = is; };
		ServerInviteSessionHandle getHandler() { return sis; };
		string getCallId() { return callId; };
		string getCfwId() { return cfwId; };

		void setNegotiated() { negotiated = true; };
		bool getNegotiated() { return negotiated; };

		bool rtpExists(string ip, uint16_t port);
		MediaCtrlRtpChannel *getRtpChannel(string label="");
		uint16_t addRtp(int pt, int media=MEDIACTRL_MEDIA_AUDIO);
		bool setRtpPeer(uint16_t localPort, const InetHostAddress &ia, uint16_t dataPort);
		bool setRtpDirection(uint16_t localPort, int direction);
		string addRtpSetting(uint16_t localPort, string value);
		void setTags(string fromTag, string toTag);
		string getFromTag();
		string getToTag();
		string getConnectionId(uint16_t localPort=0);
		string getConnectionId(string label);
		string getMediaLabel(uint16_t localPort);
		uint16_t getMediaPort(string label);
		list<string> getMediaLabels() { return rtpLabels; };

		// Callback to notify our owner
		void setCodecManager(MediaCtrlCodecManager *manager) { this->codecManager = manager; };
		MediaCtrlCodec *createCodec(int codec) { return codecManager->createCodec(codec); };
		int getBlockLen(int codec) { return codecManager->getBlockLen(codec); };

		int setSipManager(MediaCtrlSipManager *manager, string label="");
		int unsetSipManager(MediaCtrlSipManager *manager, string label="");
		/**
		* @fn payloadTypeChanged(MediaCtrlRtpChannel *rtpChannel, int pt)
		* This callback is triggered when the payload type of the specified RTP channel changes.
		* @param rtpChannel The RTP channel where the payload type changed
		* @param pt The new payload type
		*/
		void payloadTypeChanged(MediaCtrlRtpChannel *rtpChannel, int pt);
		/**
		* @fn incomingFrame(MediaCtrlRtpChannel *rtpChannel, MediaCtrlFrame *frame)
		* This callback is triggered when a new frame is received on the specified RTP channel.
		* @param rtpChannel The RTP channel from where the frame arrived
		* @param frame The received frame
		*/
		void incomingFrame(MediaCtrlRtpChannel *rtpChannel, MediaCtrlFrame *frame);
		/**
		* @fn incomingDtmf(MediaCtrlRtpChannel *rtpChannel, int type)
		* This callback is triggered when a new DTMF tone is received on the specified RTP channel.
		* @param rtpChannel The RTP channel from where the tone arrived
		* @param type The received tone, according to the enumerator
		*/
		void incomingDtmf(MediaCtrlRtpChannel *rtpChannel, int type);
		/**
		* @fn frameSent(MediaCtrlRtpChannel *rtpChannel, MediaCtrlFrame *frame)
		* This callback is triggered when a previously queued frame has actually been sent.
		* @param rtpChannel The RTP channel on which the frame was sent
		* @param frame The sent frame
		*/
		void frameSent(MediaCtrlRtpChannel *rtpChannel, MediaCtrlFrame *frame);
		/**
		* @fn channelLocked(MediaCtrlRtpChannel *rtpChannel)
		* This callback is triggered when a RTP channel has been locked by a locking frame.
		* @param rtpChannel The RTP channel being locked
		*/
		void channelLocked(MediaCtrlRtpChannel *rtpChannel);
		/**
		* @fn channelUnlocked(MediaCtrlRtpChannel *rtpChannel)
		* This callback is triggered when a RTP channel has been unlocked by an unlocking frame.
		* @param rtpChannel The RTP channel being unlocked
		*/
		void channelUnlocked(MediaCtrlRtpChannel *rtpChannel);
		/**
		* @fn channelClosed(string label)
		* This callback is triggered when a RTP channel is going to be terminated.
		* @param label The label of the RTP channel being closed
		*/
		void channelClosed(string label);

		/**
		* @fn sendBye()
		* Forces a termination of the SIP session by sending a BYE message.
		*/
		void sendBye() { is->end(); };

	private:
		bool active;
		
		bool singleManager;				// FIXME currently unused
		map<string, MediaCtrlSipManagers> sipManagers;	/*!< Who to notify about incoming frames/tones */
		ost::Mutex *mLinks;

		MediaCtrlCodecManager *codecManager;		/*!< The Codec Factory */

		bool as;					/*!< Is this an Application Server? */
		bool negotiated;				/*!< If this is an AS, has it already negotiated? */
		string callId;					/*!< The Call-ID */
		string fromTag, toTag;				/*!< Tags */
		string cfwId;					/*!< SDP cfw-id attribute */
		string connectionId;				/*!< Base Connection-ID as specified by the framework */

		ServerInviteSessionHandle sis;			/*!< The SIP server session handler */
		InviteSessionHandle is;				/*!< The SIP session handler */

		InetHostAddress address;			/*!< Local address */
		list<uint16_t> rtpPorts;			/*!< List of source media ports in this session */
		list<string> rtpLabels;				/*!< List of media labels in this session */
		map<uint16_t, MediaCtrlRtpChannel *> rtpConnectionsByPort;	/*!< Map of RTP channels in this sessions (by port) */
		map<string, MediaCtrlRtpChannel *> rtpConnectionsByLabel;	/*!< Map of RTP channels in this sessions (by label) */
};

}

#endif
