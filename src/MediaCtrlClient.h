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

#ifndef _MEDIA_CTRL_CLIENT_H
#define _MEDIA_CTRL_CLIENT_H

/*! \file
 *
 * \brief Headers: CFW Client Information Handler
 *
 * \author Lorenzo Miniero <lorenzo.miniero@unina.it>
 *
 * \ingroup core
 * \ref core
 */

#include <iostream>
#include <cc++/thread.h>
#include <poll.h>

#include "MediaCtrlMemory.h"

using namespace std;
using namespace ost;

namespace mediactrl {

/// The TlsSetup helper class
/**
* @class TlsSetup MediaCtrlClient.h
* An helper class to setup SSL with a certificate and privatekey.
*/
class TlsSetup : public gc {
	public:
		TlsSetup()
			{
				certificate = "";
				privatekey = "";
				fingerprint = "";
			};
		~TlsSetup() {};

		bool setup(string certificate, string privatekey);
		string getFingerprint() { return fingerprint; };

	private:
		string certificate, privatekey, fingerprint;
};


class MediaCtrlClient;

/// The MediaCtrlClient Callback abstract class
/**
* @class MediaCtrlClientCallback MediaCtrlClient.h
* An abstract class implemented by the CfwStack object in order to receive callback notifications related to incoming messages from this client.
*/
class MediaCtrlClientCallback {
	public:
		MediaCtrlClientCallback() {};
		~MediaCtrlClientCallback() {};

		virtual void parseMessage(MediaCtrlClient *client, string message) {};
		virtual void connectionLost(int fd) {};
};


/// CFW Client
/**
* @class MediaCtrlClient MediaCtrlClient.h
* A class handling CFW clients (i.e. Application Servers acting as clients of the Control Channel).
* @note This class does not handle transactions on behalf of the client (everything is done by the CfwStack instance), but only keeps information about it (e.g. the associated file descriptor, the client address and port, the Dialog-ID, whether it has authenticated or not and so on). Besides, the class is responsible of the Keep-Alive timer: it is up to the CfwStack instance, however, accessing the timer value to check if it has expired or not.
*/
class MediaCtrlClient : public gc, public Thread {
	public:
		MediaCtrlClient(MediaCtrlClientCallback *callback, bool tls=false, string fingerprint="");
		~MediaCtrlClient();

		/**
		* @fn run()
		* The thread implementing the protocol behaviour.
		*/
		void run();

		/**
		* @fn setDialog(string callId, string cfwId)
		* This method builds the Dialog-ID needed to correlate an CFW client to the related SIP transaction.
		* @param callId The SIP Call-ID
		* @param cfwId The SDP cfw-id attribute of the SIP session
		*/
		void setDialog(string callId, string cfwId);
		/**
		* @fn setAddress(string ip, uint16_t port)
		* Sets the transport address the client is connecting from.
		* @param ip The IP of the client
		* @param port The port the client is bound to
		*/
		void setAddress(string ip, uint16_t port);
		/**
		* @fn setFd(int fd)
		* Sets the file descriptor the client is associated with.
		* @param fd The client file descriptor
		*/
		void setFd(int fd);
		/**
		* @fn setKeepAlive(int seconds)
		* Sets the Keep-Alive value that has been negotiated through the SYNCH mechanism.
		* @param seconds The value, in seconds, of the keep-alive timeout
		* @note This method starts a timeout counter: if no keep-alive is received within that amount of seconds, the connection should be shutdown; otherwise, the counter should be resetted and started again. However, this is not done by this class, which only does the counting.
		*/
		void setKeepAlive(int seconds);
		/**
		* @fn setAuthenticated()
		* Tells the object that the client has correctly authenticated (i.e. it has sent the SYNCH message, and has correlated with the SIP transaction)
		* @note The first message a client must send is a SYNCH message, which must also correctly correlate a control channel client with the SIP transaction that originated the connection: that's why this method is used.
		*/
		void setAuthenticated() { this->authenticated = true; };

		/**
		* @fn getTimeout()
		* Checks if the keep-alive timeout has been reached or not.
		* @returns true if the keep-alive has been reached, false otherwise
		* @note This method is called by the CfwStack instance to check if a client reached the keep-alive timeout or not: if it has, it resets the counter which starts again, otherwise the connection is shutdown and the related SIP transaction terminated (or at least, that's what should happen...)
		*/
		bool getTimeout();

		/**
		* @fn getDialogId()
		* Gets the Dialog-ID that uniquely identifies the client.
		* @returns The Dialog-ID
		*/
		string getDialogId() { return dialogId; };
		/**
		* @fn getCallId()
		* Gets the Call-ID associated with the client.
		* @returns The originating SIP Call-ID
		*/
		string getCallId() { return callId; };
		/**
		* @fn getIp()
		* Gets the IP the client is connecting from.
		* @returns A string containing IP the client is connecting from
		*/
		string getIp();
		/**
		* @fn getPort()
		* Gets the port the client is connecting from.
		* @returns An unsigned short integer containing port the client is connecting from
		*/
		uint16_t getPort() { return port; };
		/**
		* @fn getFd()
		* Gets the file descriptor associated with the client.
		* @returns The client file descriptor
		*/
		int getFd() { return fd; };
		/**
		* @fn getAuthenticated()
		* Checks if the client has already authenticated or not.
		* @returns true if it has authenticated, false otherwise
		* @note This method is used by the CfwStack instance whenever a message by the client is received: in fact the first message to be received MUST be a correct SYNCH. If any other message is received before, it is rejected
		*/
		bool getAuthenticated() { return authenticated; };
		/**
		* @fn getKeepAlive()
		* Gets the Keep-Alive value that has been negotiated through the SYNCH mechanism.
		* @returns Keep-Alive value in seconds
		*/
		int getKeepAlive() { return (keepAlive/1000); };
		string getFingerprint() { return fingerprint; };
		
		int sendMessage(string message);
		string getContent(MediaCtrlClientCallback *callback, int len);

	private:
		MediaCtrlClientCallback *callback;
		string dialogId, callId;

		bool alive;
		ost::Conditional *cond;
		struct pollfd pollfds;	/*!< File descriptor to poll (client) */

		string ip;		/*!< IP this client calls from */
		uint16_t port;		/*!< Port this client is bound to */
		int fd;			/*!< The file descriptor of the CFW connection */
		bool tls;
		string fingerprint;
		void *session;
		bool accepted;

		bool authenticated;	/*!< If this client sent his SYNCH or not */

		/// For the K-Alive mechanism
		/**
		* @fn startCounter()
		* Starts the keep-alive counter, so that the timeout can be subsequently checked
		*/
		void startCounter();
		/**
		* @fn resetCounter()
		* Resets the keep-alive counter, stating that a keep-alive has been received
		*/
		void resetCounter();
		uint32_t startTime, keepAlive;	/*!< Timeout values */
		TimerPort *timer;		/*!< The timeout handler */
};

}

#endif
