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

#ifndef _MEDIA_CTRL_FRAMEWORK_H
#define _MEDIA_CTRL_FRAMEWORK_H

/*! \file
 *
 * \brief Headers: MediaCtrl Core (SIP+CFW)
 *
 * \author Lorenzo Miniero <lorenzo.miniero@unina.it>
 *
 * \ingroup core
 * \ref core
 */

// XML
#include "expat.h"

// CFW
#include "CfwStack.h"

// SIP Channel Handler
#include "MediaCtrlSip.h"

// Codecs (handled as plugins)
#include "MediaCtrlCodec.h"

// Endpoints Handler
#include "MediaCtrlEndpoint.h"

// Remote Monitor Handler
#include "RemoteMonitor.h"

#include "MediaCtrlMemory.h"

using namespace resip;
using namespace std;

#define RESIPROCATE_SUBSYSTEM Subsystem::TEST

namespace mediactrl {

/// Helper class for parsing the configuration file
/**
 * @class MediaCtrlConfValue MediaCtrl.h
 * Helper class used to parse single values in the XML configuration file: if the attribute is omitted, the element value is checked.
 */
class MediaCtrlConfValue : public gc {
	public:
		/**
		* @fn MediaCtrlConfValue(string element, string attribute="")
		* Constructor.
		* This method creates a new instance of MediaCtrlConfValue, and setups the default values.
		* @param element The XML element name in the configuration file
		* @param attribute The (optional) XML attribute name in the element
		* @note If attribute is omitted, the element value is checked
		*/
		MediaCtrlConfValue(string element, string attribute="")
			{
				scanonly = false;
				this->element = element;
				if(element == "")
					scanonly = true;
				this->package = "";
				this->attribute = attribute;
				this->result = "";
				childs.empty();
				stop = false;
				inPackage = false;
				level = 0;
			};
		/**
		* @fn ~MediaCtrlConfValue()
		* Destructor.
		*/
		~MediaCtrlConfValue() {};

		/**
		* @fn setPackage(string package)
		* This method specifies if a specific control package requested this configuration value.
		* @param package The name of the control package
		*/
		void setPackage(string package) { this->package = package; };
		/**
		* @fn string getPackage()
		* This method gets the control package interested to the configuration value.
		* @returns The name of the control package instance
		*/
		string getPackage() { return package; };
		/**
		* @fn string getResult()
		* This method returns the retrieved configuration value.
		* @returns The configuration value, or an empty string if the value was not found
		*/
		string getResult() { return result; };

		list<string> childs;	/*!< The path to the current element in the XML */
		int level;		/*!< The depth level of the current element in the XML */
		bool stop;		/*!< If we have to stop parsing or not */
		bool scanonly;		/*!< If we have to parse or only scan */
		bool inPackage;		/*!< If we are in the domain of the interested package or not */

		string package;		/*!< The name of the interested package, if any */
		string element;		/*!< The name of the requested element */
		string attribute;	/*!< The name of the requested attribute of the element, if any */
		string result;		/*!< The string containing the result */
};

/// Core
/**
 * @class MediaCtrl MediaCtrl.h
 * The class implementing the core of the MediaCtrl Prototype: it handles all SIP transactions (by extending InviteSessionHandler,
ServerRegistrationHandler and MediaCtrlSipManager) and the CFW protocol stack behaviour (by extending CfwManager).
 */
class MediaCtrl : public gc, public ThreadIf, public InviteSessionHandler, public ServerRegistrationHandler,
		public MediaCtrlSipManager, public MediaCtrlCodecManager,
		public CfwManager, public RemoteMonitorManager {
	public:
		/**
		* @fn MediaCtrl(string conf);
		* Constructor. It creates the MediaCtrl object, by setting the default values and instantiating the subordinated objects.
		* @param conf The path to the XML configuration file
		*/
		MediaCtrl(string conf);
		/**
		* @fn ~MediaCtrl();
		* Destructor. It takes care of all the cleanup process, by hanging up all ongoing SIP transaction and destroyign the subordinate objects.
		*/
		~MediaCtrl();

		/**
		* @fn int openConfiguration();
		* Opens the configuration file and checks if it's ok.
		* @returns 0 on success, -1 on failure
		*/
		int openConfiguration();
		/**
		* @fn string getConfValue(string element, string attribute="");
		* Gets a value from the XML configuration file.
		* @param element The element the value is in
		* @param attribute The specific attribute (optional) of the element
		* @returns The requested value if found, an empty string otherwise
		*/
		string getConfValue(string element, string attribute="");				// FIXME
		/**
		* @fn string getPackageConfValue(string package, string element, string attribute="");
		* Gets a value from the section specific to a package of the XML configuration file.
		* @param package The name of the package interested to the value
		* @param element The element the value is in
		* @param attribute The specific attribute (optional) of the element
		* @returns The requested value if found, an empty string otherwise
		*/
		string getPackageConfValue(string package, string element, string attribute="");	// FIXME

		/**
		* @fn codecExists(int codec);
		* Checks if a codec is supported by the framework
		* @param codec The codec identifier (the AVT profile number, usually)
		* @returns true if supported, false otherwise
		*/
		bool codecExists(int codec) { return (codecs[codec] != NULL); };
		/**
		* @fn createCodec(int codec);
		* Creates a new instance of the codec referenced by the provided identifier.
		* @param codec The codec identifier (the AVT profile number, usually)
		* @returns A pointer to the new instance, NULL if the creation failed
		*/
		MediaCtrlCodec *createCodec(int codec);
		/**
		* @fn getBlockLen(int codec);
		* Returns the block length, in bytes, of a generic frame of the specified codec (e.g. 33 for GSM)
		* @param codec The codec identifier (the AVT profile number, usually)
		* @returns The block length, -1 if the codec is not supported
		*/
		int getBlockLen(int codec);

		/**
		* @fn getEndpoint(ControlPackage *cp, string conId);
		* Requests access to the specified endpoint(connection-id/conf-id) for a package.
		* @param cp The control package which requested the endpoint
		* @param conId The requested connection-id (from~tag or from~tag~label) or conf-id
		* @returns The requested connection/conference if successful, NULL otherwise
		* @note A successful request also results in having all the events related to the endpoint (e.g. incoming frames/DTMF, locking events, etc) forwarded to the package
		*/
		MediaCtrlEndpoint *getEndpoint(ControlPackage *cp, string conId);
		/**
		* @fn createConference(ControlPackage *cp, string confId);
		* Requests the creation of a new, unique, conference endpoint to which connections might be attached to.
		* @param cp The control package which requested the creation
		* @param confId The desired unique conference identifier
		* @returns The new conference object, if successful, NULL otherwise
		* @note The confId MUST be provided, no unique random identifier is chosen by the method itself...
		*/
		MediaCtrlEndpoint *createConference(ControlPackage *cp, string confId);

		/// This callback in here is just a placeholder: it will actually be intercepted by endpoints when needed, that's why it just drops everything here.
		void payloadTypeChanged(MediaCtrlSipTransaction *sipTransaction, MediaCtrlRtpChannel *rtpChannel, int pt) { return; };
		/// This callback in here is just a placeholder: it will actually be intercepted by endpoints when needed, that's why it just drops everything here.
		void incomingFrame(MediaCtrlSipTransaction *sipTransaction, MediaCtrlRtpChannel *rtpChannel, MediaCtrlFrame *frame) { return; };
		/// This callback in here is just a placeholder: it will actually be intercepted by endpoints when needed, that's why it just drops everything here.
		void incomingDtmf(MediaCtrlSipTransaction *sipTransaction, MediaCtrlRtpChannel *rtpChannel, int type) { return; };
		/// This callback in here is just a placeholder: it will actually be intercepted by endpoints when needed, that's why it just drops everything here.
		void frameSent(MediaCtrlSipTransaction *sipTransaction, MediaCtrlRtpChannel *rtpChannel, MediaCtrlFrame *frame) { return; };
		/// This callback in here is just a placeholder: it will actually be intercepted by endpoints when needed, that's why it just drops everything here.
		void channelUnlocked(MediaCtrlSipTransaction *sipTransaction, MediaCtrlRtpChannel *rtpChannel) { return; };
		/// This callback in here is just a placeholder: it will actually be intercepted by endpoints when needed, that's why it just drops everything here.
		void channelLocked(MediaCtrlSipTransaction *sipTransaction, MediaCtrlRtpChannel *rtpChannel) { return; };
		/// This callback in here is just a placeholder: it will actually be intercepted by endpoints when needed, that's why it just drops everything here.
		void channelClosed(string connectionId, string label) { return; };

		/**
		* @fn decode(MediaCtrlFrame *frame);
		* A method to generically decode a frame: it wraps the call to the codec which will actually decode the frame.
		* @param frame The frame to decode
		* @returns The decoded frame if successful, NULL otherwise
		*/
		MediaCtrlFrame *decode(MediaCtrlFrame *frame);
		/**
		* @fn encode(MediaCtrlFrame *frame, int dstFormat);
		* A method to generically encode a frame: it wraps the call to the codec which will actually encode the frame, optionally decoding the original frame too if it's not raw.
		* @param frame The frame to encode
		* @param dstFormat The format to encode the frame to
		* @returns The encoded frame if successful, NULL otherwise
		*/
		MediaCtrlFrame *encode(MediaCtrlFrame *frame, int dstFormat);

		/**
		* @fn endDialog(string callId);
		* Adds the Call-ID to the list of ended SIP Sessions, in order to have it correctly freed subsequently.
		* @param callId The Call-ID of the terminating SIP session
		*/
		void endDialog(string callId);

		/// reSIProcate DUM stack callbacks: these callbacks implement the SIP state machine behaviour
		void onNewSession(ServerInviteSessionHandle sis, InviteSession::OfferAnswerType oat, const SipMessage& msg);
		/// reSIProcate DUM stack callbacks: these callbacks implement the SIP state machine behaviour
		void onOffer(InviteSessionHandle is, const SipMessage& msg, const SdpContents& sdp);
		/// reSIProcate DUM stack callbacks: these callbacks implement the SIP state machine behaviour
		void onConnected(InviteSessionHandle is, const SipMessage& msg);
		/// reSIProcate DUM stack callbacks: these callbacks implement the SIP state machine behaviour
		void onTerminated(InviteSessionHandle is, InviteSessionHandler::TerminatedReason reason, const SipMessage* msg);
		/// reSIProcate DUM stack callbacks: these callbacks implement the SIP state machine behaviour
		void onNewSession(ClientInviteSessionHandle cis, InviteSession::OfferAnswerType, const SipMessage& msg);
		/// reSIProcate DUM stack callbacks: these callbacks implement the SIP state machine behaviour
		void onFailure(ClientInviteSessionHandle cis, const SipMessage& msg);
		/// reSIProcate DUM stack callbacks: these callbacks implement the SIP state machine behaviour
		void onEarlyMedia(ClientInviteSessionHandle cis, const SipMessage&, const SdpContents& sdp);
		/// reSIProcate DUM stack callbacks: these callbacks implement the SIP state machine behaviour
		void onProvisional(ClientInviteSessionHandle cis, const SipMessage& msg);
		/// reSIProcate DUM stack callbacks: these callbacks implement the SIP state machine behaviour
		void onConnected(ClientInviteSessionHandle cis, const SipMessage& msg);
		/// reSIProcate DUM stack callbacks: these callbacks implement the SIP state machine behaviour
		void onForkDestroyed(ClientInviteSessionHandle cis);
		/// reSIProcate DUM stack callbacks: these callbacks implement the SIP state machine behaviour
		void onRedirected(ClientInviteSessionHandle cis, const SipMessage& msg);
		/// reSIProcate DUM stack callbacks: these callbacks implement the SIP state machine behaviour
		void onAnswer(InviteSessionHandle is, const SipMessage&, const SdpContents& sdp);
		/// reSIProcate DUM stack callbacks: these callbacks implement the SIP state machine behaviour
		void onOfferRequired(InviteSessionHandle is, const SipMessage& msg);
		/// reSIProcate DUM stack callbacks: these callbacks implement the SIP state machine behaviour
		void onOfferRejected(InviteSessionHandle is, const SipMessage* msg);
		/// reSIProcate DUM stack callbacks: these callbacks implement the SIP state machine behaviour
		void onInfo(InviteSessionHandle is, const SipMessage& msg);
		/// reSIProcate DUM stack callbacks: these callbacks implement the SIP state machine behaviour
		void onInfoSuccess(InviteSessionHandle is, const SipMessage& msg);
		/// reSIProcate DUM stack callbacks: these callbacks implement the SIP state machine behaviour
		void onInfoFailure(InviteSessionHandle is, const SipMessage& msg);
		/// reSIProcate DUM stack callbacks: these callbacks implement the SIP state machine behaviour
		void onMessage(InviteSessionHandle is, const SipMessage& msg);
		/// reSIProcate DUM stack callbacks: these callbacks implement the SIP state machine behaviour
		void onMessageSuccess(InviteSessionHandle is, const SipMessage& msg);
		/// reSIProcate DUM stack callbacks: these callbacks implement the SIP state machine behaviour
		void onMessageFailure(InviteSessionHandle is, const SipMessage& msg);
		/// reSIProcate DUM stack callbacks: these callbacks implement the SIP state machine behaviour
		void onRefer(InviteSessionHandle is, ServerSubscriptionHandle, const SipMessage& msg);
		/// reSIProcate DUM stack callbacks: these callbacks implement the SIP state machine behaviour
		void onReferNoSub(InviteSessionHandle is, const SipMessage& msg);
		/// reSIProcate DUM stack callbacks: these callbacks implement the SIP state machine behaviour
		void onReferRejected(InviteSessionHandle is, const SipMessage& msg);
		/// reSIProcate DUM stack callbacks: these callbacks implement the SIP state machine behaviour
		void onReferAccepted(InviteSessionHandle is, ClientSubscriptionHandle, const SipMessage& msg);

		/// reSIProcate DUM stack callbacks: called when registration is refreshed
		void onRefresh(ServerRegistrationHandle, const SipMessage& reg);
		/// reSIProcate DUM stack callbacks: called when one or more specified contacts is removed
		void onRemove(ServerRegistrationHandle, const SipMessage& reg);
		/// reSIProcate DUM stack callbacks: called when all the contacts are removed using "Contact: *"
		void onRemoveAll(ServerRegistrationHandle, const SipMessage& reg);
		/// reSIProcate DUM stack callbacks: called when one or more contacts are added. This is after authentication has all succeeded
		void onAdd(ServerRegistrationHandle, const SipMessage& reg);
		/// reSIProcate DUM stack callbacks: called when a client queries for the list of current registrations
		void onQuery(ServerRegistrationHandle, const SipMessage& reg);

		/// Remote Monitor
		/**
		* @fn remoteMonitorQuery(RemoteMonitor *monitor, RemoteMonitorRequest *request);
		* This callback is signalled whenever a remote monitor makes an auditing request.
		* @param monitor The RemoteMonitor which made the request
		* @param request The RemoteMonitorRequest the monitor made
		* @returns 0 on success, -1 otherwise
		*/
		int remoteMonitorQuery(RemoteMonitor *monitor, RemoteMonitorRequest *request);

	private:
		string configurationFile;		/*!< The path to the XML configuration file */
		string configuration;			/*!< The whole content of the XML configuration file */

		/**
		* @fn loadCodecs();
		* Loads all the codec plugins (*.so) from the codecs folder
		*/
		void loadCodecs();

		/**
		* @fn thread()
		* The thread handling all the core behaviour.
		*/
		void thread();

		bool alive;				/*!< Whether the Core is alive or not (false=shutting down) */
		bool acceptCalls;			/*!< Whether the SIP hander must accept incoming INVITEs or not (false=shutting down) */

		MasterProfile *profile;			/*!< The reSIProcate profile for the SIP and DUM Stacks */
		SipStack *sip;				/*!< reSIProcate SIP (Session Initiation Protocol) stack */
		DialogUsageManager *dum;		/*!< reSIProcate Dialog User Manager */
		StackThread *sipThread;			/*!< SIP thread */
		DumThread *dumThread;			/*!< DUM thread */
		string sipName;				/*!< The User part of the contact URI */
		string sipAddressString;		/*!< The IP */
		InetHostAddress sipAddress;		/*!< The public address */
		unsigned short int sipPort;		/*!< The SIP listening port (UDP) */
		NameAddr contact;			/*!< The SIP contact for the MediaCtrl SIP server */

		/**
		* @fn handleSipMessage(SipMessage *received);
		* Method that implements the behaviour related to incoming SIP INVITEs.
		* @param received The received SIP message
		*/
		void handleSipMessage(SipMessage* received);

		ost::Mutex mSip;			/*!< Mutex for SIP transactions */
		map<string, MediaCtrlSipTransaction *>sipTransactions;	/*!< Map of SIP Transactions (call-id) */
		map<string, MediaCtrlSipTransaction *>sipConnections;	/*!< Map of SIP Transactions(connection-id) */
		map<InviteSessionHandler *, MediaCtrlSipTransaction *>sipHandlers;	/*!< Map of SIP Transactions (handler) */
		map<string, MediaCtrlConnection *>endpointConnections;	/*!< Map of Endpoints (connection-id) */
		map<string, MediaCtrlConference *>endpointConferences;	/*!< Map of Endpoints (conf-id) */
		map<int, CodecFactory *>codecs;		/*!< Map of Codecs (factories) */
		map<int, MediaCtrlCodec *>codecPlugins;	/*!< Map of Codecs (plugins) */
		list<void *>codecSharedObjects;		/*!< List of handles to the codec shared objects */

		CfwStack *cfw;			/*!< CFW (Media Server Control Protocol) stack */
		InetHostAddress cfwAddress;		/*!< The public address */
		unsigned short int cfwPort;		/*!< The CFW listening port (TCP) */
		uint16_t cfwRestrict[4];				/*!< Only accept AS from this range (default=0.0.0.0) */
		TlsSetup *tls;		/*!< The helper class handling SSL support for the CFW stack */

		RemoteMonitor *monitor;			/*!< A socket interface to let remote monitors query us about the current state */
		unsigned short int monitorPort;		/*!< The monitor listening port (TCP) */
};

}

#endif
