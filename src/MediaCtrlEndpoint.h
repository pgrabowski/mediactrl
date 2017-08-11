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

#ifndef _MEDIA_CTRL_ENDPOINT_H
#define _MEDIA_CTRL_ENDPOINT_H

/*! \file
 *
 * \brief Headers: Wrapper for Handled Endpoints (connections+conferences)
 *
 * \author Lorenzo Miniero <lorenzo.miniero@unina.it>
 *
 * \ingroup core
 * \ref core
 */

// SIP/RTP
#include "MediaCtrlSip.h"

// Control Packages (for abstract connection handlers)
#include "ControlPackage.h"

#include "MediaCtrlMemory.h"

namespace mediactrl {

/// MediaCtrlEndpoint type
enum {
	/*! Connection */
	MEDIACTRL_CONNECTION = 0,
	/*! Conference */
	MEDIACTRL_CONFERENCE,
};

// Endpoint (base class for both connections and conferences)
class MediaCtrlEndpointManager;
/// Endpoint base class
/**
* @class MediaCtrlEndpoint MediaCtrlEndpoint.h
* Abstract class for handling endpoint media connections and conferences: this class wraps SIP and RTP, acting as a listener to their events (e.g. incoming frames).
*/
class MediaCtrlEndpoint : public gc, public MediaCtrlSipManager {
	public:
		MediaCtrlEndpoint() {}
		MediaCtrlEndpoint(string Id, string label="") {}
		virtual ~MediaCtrlEndpoint() {}

		/**
		* @fn getType()
		* Gets the endpoint type, considering this class is abstract.
		* @returns The endpoint type (MEDIACTRL_CONNECTION or MEDIACTRL_CONFERENCE)
		*/
		int getType() { return type; };
		/**
		* @fn getId()
		* Gets the string identifier associated with the endpoint.
		* @returns The identifier associated with the endpoint (e.g. the connection-id or the conf-id)
		*/
		string getId() { return Id; };
		virtual int getFormat(int mediaType=MEDIACTRL_MEDIA_UNKNOWN) = 0;
		/**
		* @fn getMediaType()
		* Gets the endpoint media type, considering this class is abstract.
		* @returns The endpoint media type (MEDIACTRL_MEDIA_UNKNOWN|AUDIO)
		*/
		int getMediaType() { return media; };

		/**
		* @fn getSip()
		* Gets the SIP transaction this endpoint belongs to.
		* @returns The MediaCtrlSipTransaction, if it exists, NULL otherwise
		*/
		MediaCtrlSipTransaction *getSip() { return sipTransaction; };
		/**
		* @fn getRtp()
		* Gets the RTP channel this endpoint is associated with.
		* @returns The MediaCtrlRtpChannel, if it exists, NULL otherwise
		*/
		MediaCtrlRtpChannel *getRtp() { return rtpChannel; };

		/**
		* @fn setCpConnection(ControlPackageConnection *cpConnection)
		* Sets the ControlPackageConnection instance associated with the endpoint.
		* @param cpConnection The ControlPackageConnection instance
		* @note This association is needed because the control package plugins can't directly access endpoints, and so access those instead as wrappers and listeners.
		*/
		void setCpConnection(ControlPackageConnection *cpConnection) { this->cpConnection = cpConnection; };
		/**
		* @fn getCpConnection()
		* Gets the ControlPackageConnection instance associated with the endpoint.
		* @returns A pointer to the ControlPackageConnection instance
		*/
		ControlPackageConnection *getCpConnection() { return cpConnection; };

		virtual uint32_t getFlags(int mediaType=MEDIACTRL_MEDIA_UNKNOWN) = 0;
		virtual void setLocal(MediaCtrlSipTransaction *sipTransaction, MediaCtrlRtpChannel *rtpChannel) = 0;

		virtual void clearDtmfBuffer() = 0;
		virtual int getNextDtmfBuffer() = 0;
		virtual void sendFrame(MediaCtrlFrame *frame) = 0;

		virtual void increaseCounter() = 0;
		virtual void decreaseCounter() = 0;

	protected:
		int type;					/*!< Connection/Conference */
		string Id;					/*!< connection-id/conf-id */
		string label;					/*!< label */
		int media;					/*!< Media type (audio/video), if a specific connection */

		ControlPackageConnection *cpConnection;		/*!< Who must receive incoming data */

		// Data for this endpoint
		MediaCtrlSipTransaction *sipTransaction;	/*!< SIP Session this endpoint belongs to, if any */
		MediaCtrlRtpChannel *rtpChannel;		/*!< RTP Channel associated with this endpoint, if any */
};
/// List of MediaCtrlEndpoint instances
typedef list<MediaCtrlEndpoint *> MediaCtrlEndpoints;

/// Endpoint: Connections
/**
* @class MediaCtrlConnection MediaCtrlEndpoint.h
* A wrapper class, extending MediaCtrlEndpoint, which can be associated either with a generic connection-id (from~to) or with a specific connection-id (from~to/label). A generic connection associated with a SIP transactions is always a wrapper to the specific underlying connection-ids (the RTP channels of the session).
*/
class MediaCtrlConnection : public MediaCtrlEndpoint {
	public:
		MediaCtrlConnection();
		MediaCtrlConnection(string connectionId, string label="");
		~MediaCtrlConnection();

		/**
		* @fn setLocal(MediaCtrlSipTransaction *sipTransaction, MediaCtrlRtpChannel *rtpChannel)
		* Sets the SIP session and (optionally) the RTP channel associated with this endpoint connection.
		* @param sipTransaction The SIP session
		* @param rtpChannel The RTP channel
		* @note This method also adds the endpoint as a listener for all the events related to events from both the parameter. Precisely, in case the RTP channel is specified, only events from that channel will be received; otherwise, the endpoint will receive events from all the RTP channels handled by the SIP session, acting as a "macro" connection
		*/
		void setLocal(MediaCtrlSipTransaction *sipTransaction, MediaCtrlRtpChannel *rtpChannel);

		/**
		* @fn getFormat(int mediaType)
		* Gets the format (payload type) specifying the type of media if needed (one endpoint might wrap two media, audio and video)
		* @returns The format (payload type) of the media flowing through specified RTP channel.
		*/
		int getFormat(int mediaType);
		/**
		* @fn getLabel()
		* Gets the label of the media flowing through the only associated RTP channel.
		* @returns The label of the media flowing through the only associated RTP channel; if more channels are associated with the connection, it means that the connection-id is in the 'from~tag' form, and so an empty string is returned
		*/
		string getLabel() { return label; };
		/**
		* @fn getFlags(int mediaType)
		* Gets the flags associated with the endpoint, specifying the type of media if needed (one endpoint might wrap two media, audio and video)
		* @returns The flags mask
		*/
		uint32_t getFlags(int mediaType=MEDIACTRL_MEDIA_UNKNOWN);

		/**
		* @fn clearDtmfBuffer()
		* Clears the DTMF tones buffer of all the (or the only) associated RTP channel(s).
		*/
		void clearDtmfBuffer();
		/**
		* @fn getNextDtmfBuffer()
		* Gets the first DTMF tone from the head of the DTMF tones buffer of the first (maybe only) associated RTP channel.
		* @note In case more RTP channels are associated with this endpoint (i.e. the endpoint is associated with the whole SIP session, which negotiated many media) only the first RTP channel is queried for the tone. This is because most of the times the audio channel is the first one that is negotiated. It is probably a buggy behaviour, but since this prototype currently handles only audio anyway who cares...
		*/
		int getNextDtmfBuffer();
		/**
		* @fn sendFrame(MediaCtrlFrame *frame)
		* Sends a frame on all the (or the only) associated RTP channel(s).
		* @param frame The frame to be sent
		*/
		void sendFrame(MediaCtrlFrame *frame);

		/**
		* @fn payloadTypeChanged(MediaCtrlSipTransaction *sipTransaction, MediaCtrlRtpChannel *rtpChannel, int pt)
		* This callback is triggered when the payload type changes.
		* @param sipTransaction The SIP session interested by the event
		* @param rtpChannel The RTP channel interested by the event
		* @param pt The new payload type
		*/
		void payloadTypeChanged(MediaCtrlSipTransaction *sipTransaction, MediaCtrlRtpChannel *rtpChannel, int pt);
		/**
		* @fn incomingFrame(MediaCtrlSipTransaction *sipTransaction, MediaCtrlRtpChannel *rtpChannel, MediaCtrlFrame *frame)
		* This callback is triggered when a new frame is received on the specified channel.
		* @param sipTransaction The SIP session interested by the event
		* @param rtpChannel The RTP channel interested by the event
		* @param frame The received frame
		*/
		void incomingFrame(MediaCtrlSipTransaction *sipTransaction, MediaCtrlRtpChannel *rtpChannel, MediaCtrlFrame *frame);
		/**
		* @fn incomingDtmf(MediaCtrlSipTransaction *sipTransaction, MediaCtrlRtpChannel *rtpChannel, int type)
		* This callback is triggered when a new DTMF tone is received on the specified channel.
		* @param sipTransaction The SIP session interested by the event
		* @param rtpChannel The RTP channel interested by the event
		* @param type The received DTMF tone
		*/
		void incomingDtmf(MediaCtrlSipTransaction *sipTransaction, MediaCtrlRtpChannel *rtpChannel, int type);
		/**
		* @fn frameSent(MediaCtrlSipTransaction *sipTransaction, MediaCtrlRtpChannel *rtpChannel, MediaCtrlFrame *frame)
		* This callback is triggered when a previously queued frame has been actually sent on the specified channel.
		* @param sipTransaction The SIP session interested by the event
		* @param rtpChannel The RTP channel interested by the event
		* @param frame The sent frame
		*/
		void frameSent(MediaCtrlSipTransaction *sipTransaction, MediaCtrlRtpChannel *rtpChannel, MediaCtrlFrame *frame);
		/**
		* @fn channelLocked(MediaCtrlSipTransaction *sipTransaction, MediaCtrlRtpChannel *rtpChannel)
		* This callback is triggered when the specified channel gets locked.
		* @param sipTransaction The SIP session interested by the event
		* @param rtpChannel The RTP channel interested by the event
		*/
		void channelLocked(MediaCtrlSipTransaction *sipTransaction, MediaCtrlRtpChannel *rtpChannel);
		/**
		* @fn channelUnlocked(MediaCtrlSipTransaction *sipTransaction, MediaCtrlRtpChannel *rtpChannel)
		* This callback is triggered when the specified channel gets unlocked.
		* @param sipTransaction The SIP session interested by the event
		* @param rtpChannel The RTP channel interested by the event
		*/
		void channelUnlocked(MediaCtrlSipTransaction *sipTransaction, MediaCtrlRtpChannel *rtpChannel);
		/**
		* @fn channelClosed(string connectionId, string label)
		* This callback is triggered when the specified channel is about to get closed.
		* @param connectionId The connection identifier (from~to) of the SIP session interested by the event
		* @param label The media label of the RTP channel interested by the event
		*/
		void channelClosed(string connectionId, string label);

		// Wrapping methods, for owners and individual connections
		/**
		* @fn addOwned(MediaCtrlConnection *connection)
		* Adds an owned connection (from~to/label) to this wrapping connection (from~to).
		* @param connection The connection to own
		* @returns 0 on success, -1 otherewise (e.g. if this connection is not a wrapping one)
		* @note This method also causes this connection to be add as a listener of events related to the owned connection; in the same way, the owned connection will be triggered by the wrapper when needed (e.g. when a frame is to be sent on all the owned connections)
		*/
		int addOwned(MediaCtrlConnection *connection);
		/**
		* @fn removeOwned(MediaCtrlConnection *connection)
		* Removes an owned connection (from~to/label) from this wrapping connection (from~to).
		* @param connection The connection not to own anymore
		* @returns 0 on success, -1 otherewise (e.g. if this connection is not a wrapping one)
		* @note This method also causes this connection to be removed as a listener of events related to the owned connection
		*/
		int removeOwned(MediaCtrlConnection *connection);
		/**
		* @fn getOwned(string ownedLabel)
		* Gets a pointer to the owned connection identified by the provided label, if the called instance is a wrapper.
		* @param ownedLabel Media label of the owned connection (from~to/ownedLabel)
		* @returns A pointer to the MediaCtrlConnection instance associated with the requested owned connection, if successful, NULL otherwise (e.g. if the called instance is not a wrapper, or the requested connection is not owned)
		*/
		MediaCtrlConnection *getOwned(string ownedLabel);
		/**
		* @fn getOwned(int mediaType)
		* Gets a pointer to the owned connection identified by the provided media type, if the called instance is a wrapper.
		* @param mediaType Media type of the owned connection (e.g. MEDIACTRL_MEDIA_AUDIO)
		* @returns A pointer to the MediaCtrlConnection instance associated with the requested owned connection, if successful, NULL otherwise (e.g. if the called instance is not a wrapper, or the requested connection is not owned)
		* @note This method assumes only one RTP connection per media is available
		*/
		MediaCtrlConnection *getOwned(int mediaType);
		/**
		* @fn setOwner(MediaCtrlConnection *connection)
		* Sets the MediaCtrlConnection instance that will act as a wrapper to the called instance.
		* @param connection The wrapping connection
		* @returns 0 on success, -1 otherwise (e.g. if the provided connection is not a wrapper, or if the called instance cannot be owned)
		*/
		int setOwner(MediaCtrlConnection *connection);
		/**
		* @fn getOwner()
		* Gets a pointer to the connection wrapping the called instance, if any.
		* @returns A pointer to the MediaCtrlConnection instance associated with the requested wrapper, if successful, NULL otherwise (e.g. if the called instance is not owned)
		*/
		MediaCtrlConnection *getOwner();

		void incomingFrame(ControlPackageConnection *subConnection, MediaCtrlFrame *frame);
		void incomingDtmf(ControlPackageConnection *subConnection, int type);
		void frameSent(ControlPackageConnection *subConnection, MediaCtrlFrame *frame);
		void channelLocked(ControlPackageConnection *subConnection);
		void channelUnlocked(ControlPackageConnection *subConnection);
		void channelClosed(ControlPackageConnection *subConnection, string connectionId);

		void increaseCounter();
		void decreaseCounter();

	private:
		list<MediaCtrlConnection *>channels;	/*!< Individual channels, if this is a wrapper */
		MediaCtrlConnection *owner;		/*!< Pointer to the Wrapper, if this is an individual channel */

		uint16_t counter;			/*!< Reference counter of entities using this connection */
};

/// Endpoint: Conferences
/**
* @class MediaCtrlConference MediaCtrlEndpoint.h
* A wrapper class, extending MediaCtrlEndpoint, which can be associated with a generic conf-id.
*/
class MediaCtrlConference : public MediaCtrlEndpoint {
	public:
		MediaCtrlConference();
		MediaCtrlConference(string confId, string label="");
		~MediaCtrlConference();

		/// Unused
		int getFormat(int mediaType) { return -1; };		// FIXME

		/// Unused
		void clearDtmfBuffer() { return; };	// FIXME
		/// Unused
		int getNextDtmfBuffer() { return -1; };	// FIXME

		void sendFrame(MediaCtrlFrame *frame);
		void incomingFrame(MediaCtrlFrame *frame);

		/// Unused
		void payloadTypeChanged(MediaCtrlSipTransaction *sipTransaction, MediaCtrlRtpChannel *rtpChannel, int pt) { return; };
		/// Unused
		void incomingFrame(MediaCtrlSipTransaction *sipTransaction, MediaCtrlRtpChannel *rtpChannel, MediaCtrlFrame *frame) { return; };
		/// Unused
		void incomingDtmf(MediaCtrlSipTransaction *sipTransaction, MediaCtrlRtpChannel *rtpChannel, int type) { return; };
		/// Unused
		void frameSent(MediaCtrlSipTransaction *sipTransaction, MediaCtrlRtpChannel *rtpChannel, MediaCtrlFrame *frame) { return; };
		/// Unused
		void channelLocked(MediaCtrlSipTransaction *sipTransaction, MediaCtrlRtpChannel *rtpChannel) { return; };
		/// Unused
		void channelUnlocked(MediaCtrlSipTransaction *sipTransaction, MediaCtrlRtpChannel *rtpChannel) { return; };
		/// Unused
		void channelClosed(string connectionId, string label) { return; };

		/// Unused
		void setLocal(MediaCtrlSipTransaction *sipTransaction, MediaCtrlRtpChannel *rtpChannel) { return; };

		/// Unused (FIXME)
		uint32_t getFlags(int mediaType=MEDIACTRL_MEDIA_UNKNOWN);

		/// Unused
		void increaseCounter() { return; };
		void decreaseCounter() { return; };
};

}

#endif
