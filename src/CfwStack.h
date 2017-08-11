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

#ifndef _CFW_STACK_H
#define _CFW_STACK_H

/*! \file
 *
 * \brief Headers: CFW Protocol Stack and Handler
 *
 * \author Lorenzo Miniero <lorenzo.miniero@unina.it>
 *
 * \ingroup core
 * \ref core
 */

// MediaCtrl
#include "MediaCtrlClient.h"

// Endpoints
#include "MediaCtrlEndpoint.h"

// Control Packages (handled as plugins)
#include "ControlPackage.h"

#include "MediaCtrlMemory.h"

using namespace std;


namespace mediactrl {

class CfwStack;

/// CFW Listener and Manager
/**
* @class CfwManager CfwStack.h
* An abstract class implemented by the core MediaCtrl object in order to receive callback notifications related to requests made by the packages plugins.
*/
class CfwManager {
	public:
		CfwManager() {};
		virtual ~CfwManager() {};

		virtual void endDialog(string callId) = 0;

		virtual MediaCtrlEndpoint *getEndpoint(ControlPackage *cp, string conId) = 0;
		virtual MediaCtrlEndpoint *createConference(ControlPackage *cp, string confId="") = 0;

		virtual MediaCtrlFrame *decode(MediaCtrlFrame *frame) = 0;
		virtual MediaCtrlFrame *encode(MediaCtrlFrame *frame, int dstFormat) = 0;

		virtual string getConfValue(string element, string attribute="") = 0;				// FIXME
		virtual string getPackageConfValue(string package, string element, string attribute="") = 0;	// FIXME
};


/// CFW Framework Message
/**
* @class CfwMessage CfwStack.h
* A helper class to handle single messages of an ongoing transaction.
*/
class CfwMessage : public gc {
	public:
		/**
		* @fn CfwMessage(int seq, string text)
		* Constructor. Builds an CfwMessage instance out of the provided text content, and with the specified sequence number.
		*/
		CfwMessage(int seq, string text);
		/**
		* @fn ~CfwMessage()
		* Destructor.
		*/
		~CfwMessage();

		/**
		* @fn dontWait()
		* States that this message doesn't expect a 200 ACK from the AS.
		*/
		void dontWait() { waitingForAck = false; };
		/**
		* @fn setSent()
		* States that this message has been correctly sent to the AS.
		*/
		void setSent() { sent = true; };

		/**
		* @fn getSent()
		* Gets the Sequence Number of this message in the transaction.
		* @returns The Sequence Number in the transaction, -1 if the message has no sequence number
		*/
		int getSeq() { return seq; };
		/**
		* @fn isWaiting()
		* Checks if this message expects a 200 ACK from the AS.
		* @returns true if it's waiting, false otherwise
		*/
		bool isWaiting() { return waitingForAck; };
		/**
		* @fn getSent()
		* Checks if this message has already been sent or not.
		* @returns true if it's been sent, false otherwise
		*/
		bool getSent() { return sent; };
		/**
		* @fn getText()
		* Gets the full text of the message.
		* @returns The string containing the content of the message
		*/
		string getText() { return text; };

	private:
		bool sent;			/*!< Was this message sent? */
		int seq;			/*!< The Sequence Number */
		string text;			/*!< The full message */
		bool waitingForAck;		/*!< Does this message require a 200 ACK by the AS? */
};


/// CFW Framework Transaction
/**
* @class CfwTransaction CfwStack.h
* A helper class to handle ongoing transactions: each transaction instance is a thread handling its own state.
*/
class CfwTransaction : public gc, public Thread {
	public:
		/**
		* @fn CfwTransaction(CfwStack *cfw, MediaCtrlClient *client, string tid)
		* Constructor. Takes note of the CfwStack instance handling the transaction, which in turn is identified by the tid string.
		* @param cfw The CfwStack instance handling the transaction
		* @param client The CFW client
		* @param tid The transaction identifier
		*/
		CfwTransaction(CfwStack *cfw, MediaCtrlClient *client, string tid);
		/**
		* @fn ~CfwTransaction()
		* Destructor.
		*/
		~CfwTransaction();

		/**
		* @fn setClient(MediaCtrlClient *client)
		* Sets the the listening CFW client to reply to.
		* @param client The CFW client
		*/
		void setClient(MediaCtrlClient *client) { this->client = client; };
		/**
		* @fn getClient()
		* Gets the the listening CFW client to reply to.
		* @returns The CFW client
		*/
		MediaCtrlClient *getClient() { return client; };
		/**
		* @fn getTransaction()
		* Gets the transaction identifier.
		* @returns The transaction identifier
		*/
		string getTransaction() { return tid; };
		/**
		* @fn getStatus()
		* Gets the current state of the transaction (e.g. pending).
		* @returns The current state of the transaction in the control package processing
		*/
		int getStatus() { return status; };

		void startTimer();

		/**
		* @fn report(int status, int timeout, string mime="", string blob="")
		* This method sends a REPORT message in the transaction.
		* @param status The status to put in the REPORT (e.g. CP_REPORT_TERMINATE --> "terminate")
		* @param timeout The timeout value to set in the message
		* @param mime A string addressing the MIME type of the Control Package sending the report
		* @param blob The (optional) XML payload, provided by the control package
		*/
		int report(int status, int timeout, string mime="", string blob="");
		/**
		* @fn control(string cp, string mime, string blob)
		* This method sends a CONTROL notification message in the transaction.
		* @param cp A string addressing the Control Package sending the notification
		* @param mime A string addressing the MIME type of the Control Package sending the notification
		* @param blob The XML payload, provided by the control package
		*/
		int control(string cp, string mime, string blob);
		/**
		* @fn errorCode(int code, string header="", string blob="")
		* This method reports an error in the transaction.
		* @param code The error code of the message (e.g. 403 --> Forbidden)
		* @param header The (optional) additional header lines
		* @param blob The (optional) XML payload, provided by the control package
		*/
		int errorCode(int code, string header="", string blob="");

		/**
		* @fn ackReceived(int seq)
		* Triggers that a 200 ACK to the specified message (identified by the sequence number in the transaction) has been received.
		* @param seq The sequence number in the transaction
		*/
		void ackReceived(int seq);

	private:
		/**
		* @fn run()
		* The thread implementing the transaction state behaviour.
		*/
		void run();
		/**
		* @fn queueMessage(CfwMessage *msg, int newstatus)
		* Queues a message (an CfwMessage instance) to send it later.
		* @param msg The message to queue in order to have it sent
		* @param newstatus The status of the transaction after this message
		*/
		void queueMessage(CfwMessage *msg, int newstatus=-1);

		CfwStack *cfw;		/*!< CfwStack instance handling the transaction */
		bool running;			/*!< Whether the transaction is running or not */
		bool active;			/*!< Whether the transaction is running or not */
		TimerPort *timeoutTimer;	/*!< Timer used to check timeout, so that we send refresh updates accordingly */
		uint32_t timeoutStart;		/*!< The reference start time for each timeout */

		MediaCtrlClient *client;		/*!< AS */
		string tid;			/*!< Transaction-id */
		int seq;			/*!< Next Sequence number */
		int status;			/*!< Status of the transaction */
		list<CfwMessage *>messages;	/*!< List of outgoing messages */
		list<CfwMessage *>oldMessages;		/*!< List of old messages to destroy */
		ost::Mutex mMsg;				/*!< Mutex for the lists */
};


/// CFW Protocol Stack and Behaviour
/**
* @class CfwStack CfwStack.h
* The class implementing the CFW protocol stack: a thread is responsible of all incoming messages, and of handling transactions with the interested control packages.
* @note The class also does (as it probably shouldn't, but whatever...) something more than that: all the interactions between the control package plugins and the core are wrapped by it, and then forwarded to the core MediaCtrl instance through the CfwManager abstract methods. This includes requests to (indirectly) access media connections, conferences, codecs and so on.
*/
class CfwStack : public gc, public ThreadIf, public ControlPackageCallback, public MediaCtrlClientCallback {
	public:
		CfwStack();
		CfwStack(InetHostAddress &address, unsigned short int port, bool hardKeepAlive=true);
		~CfwStack();

		/**
		* @fn thread()
		* The thread implementing the protocol behaviour.
		*/
		void thread();

		/**
		* @fn transactionEnded(string tid)
		* This method informs the stack that the transaction identified by 'tid' can be freed.
		* @param tid The identifier of the closed transaction
		* @note The transaction is not freed/removed here, this is just a notification to the stack, which will handle it later on
		*/
		void transactionEnded(string tid);

		/**
		* @fn setCfwManager(CfwManager *manager)
		* Sets the callback to the object which will handle all CFW-related events (i.e. the MediaCtrl core object).
		* @param manager The event listener and handler
		*/
		void setCfwManager(CfwManager *manager);
		/**
		* @fn getInfo(string about)
		* This method implements the auditing response related to CFW (including those about control packages).
		* @param about A string addressing the audit request (e.g. "cfw all" for all information related to the CFW state)
		* @returns A string containing the requested information
		* @note Currently the supported requests are "cfw all" (all information), "cfw transactions" (just info about transactions), "cfw clients" (info about the control channel clients) and "packagename" (e.g. "msc-ivr-basic" to receive info about the state of the IVR package)
		*/
		string getInfo(string about);

		/**
		* @fn getPort()
		* Gets the port the CFW server is listening on.
		* @returns The port the CFW server is listening on
		*/
		unsigned short int getPort() { return port; };

		/**
		* @fn addClient(string callId, string cfwId, string ip, uint16_t port, bool tls, string fingerprint)
		* Adds a new CFW client, passing its source transport address and related SIP identifiers.
		* @param callId The SIP Call-ID header identifier
		* @param cfwId The SDP cfw-id identifier
		* @param ip The IP the client is connecting from
		* @param port The port the client is connecting from
		* @param tls TCP or TLS? (default TCP)
		* @param fingerprint The negotiated fingerprint, if TLS is involved
		* @returns 0 on success, -1 otherwise
		*/
		int addClient(string callId, string cfwId, string ip, uint16_t port, bool tls=false, string fingerprint="");
		/**
		* @fn removeClient(string cfwId)
		* Removes an existing CFW client, addressed by the provided SIP identifiers.
		* @param cfwId The SDP cfw-id identifier
		* @returns 0 on success, -1 otherwise
		*/
		int removeClient(string cfwId);
		/**
		 * @fn getClient(int fd)
		 * Gets the MediaCtrlClient instance associated with a file descriptor
		 * @param fd The file descriptor
		 * @returns The MediaCtrlClient instance, if available, NULL otherwise
		 */
		MediaCtrlClient *getClient(int fd);

		/**
		* @fn getPackages()
		* Gets the list of loaded packages as strings.
		* @returns The list of loaded packages
		*/
		list<string> getPackages();
		/**
		* @fn matchPackages(string pkgs, bool *ok)
		* Checks if the requested control packages are available and/or supported, building the reply buffer accordingly.
		* @param pkgs A single string containing the list of packages requested in a SYNCH message
		* @param ok An input/output boolean value to report the success or failure of the packages match
		* @returns A string containing the required and/or supported packages, according to the boolean flag
		*/
		string matchPackages(string pkgs, bool *ok);
		/**
		* @fn matchConnection(int fd, struct sockaddr_in *client)
		* Checks if the file descriptor is associated with the socket address.
		* @param fd The file descriptor to check
		* @param client The client socket address structure
		* @returns 0 on success, -1 otherwise
		*/
		int matchConnection(int fd, struct sockaddr_in *client);

		/**
		* @fn report(ControlPackage *cp, void *sender, string tid, int status, int timeout, string blob)
		* This callback is triggered any time a package wants to send a REPORT message.
		* @param cp The ControlPackage willing to send the REPORT
		* @param sender The MediaCtrlClient which originated the request in the first place
		* @param tid The transaction identifier this REPORT belongs to
		* @param status The status to put in the REPORT (e.g. CP_REPORT_TERMINATE --> "terminate")
		* @param timeout The timeout value to set in the message
		* @param blob The (optional) XML payload, provided by the control package
		*/
		void report(ControlPackage *cp, void *sender, string tid, int status, int timeout, string blob);
		/**
		* @fn control(ControlPackage *cp, void *sender, string blob)
		* This callback is triggered any time a package wants to send a CONTROL message (event notification).
		* @param cp The ControlPackage willing to send the CONTROL
		* @param sender The MediaCtrlClient which originated the request in the first place
		* @param blob The XML payload, provided by the control package
		*/
		void control(ControlPackage *cp, void *sender, string blob);
		/**
		* @fn getConnection(ControlPackage *cp, string conId);
		* Requests access to the specified connection(connection-id/conf-id) for a package.
		* @param cp The control package which requested the endpoint
		* @param conId The requested connection-id (from~tag) or conf-id
		* @returns The requested connection/conference if successful, NULL otherwise
		* @note A successful request also results in having all the events related to the endpoint (e.g. incoming frames/DTMF, locking events, etc) forwarded to the package. The method actually wraps a call to the event manager, and returns accordingly to the result of this call
		*/
		ControlPackageConnection *getConnection(ControlPackage *cp, string conId);
		/**
		* @fn createConference(ControlPackage *cp, string confId);
		* Requests the creation of a new, unique, conference connection to which connections might be attached to.
		* @param cp The control package which requested the creation
		* @param confId The desired unique conference identifier
		* @returns The new conference object, if successful, NULL otherwise
		* @note The confId MUST be provided, no unique random identifier is chosen by the method itself... The method actually wraps a call to the event manager, and returns accordingly to the result of this call
		*/
		ControlPackageConnection *createConference(ControlPackage *cp, string confId);

		void dropConnection(ControlPackage *cp, ControlPackageConnection *connection);

		/**
		* @fn getPackageConfValue(ControlPackage *cp, string element, string attribute="")
		* Retrieves a value from the XML configuration file in the section related to a specific package
		* @param cp The package interested in the value
		* @param element The XML element the requested value is in
		* @param attribute The (optional) attrbute of the element containing the value
		* @returns The requested value if present, an empty string otherwise
		* @note This method only does the search in the subsection of the configuration file related to the specfic requesting package (Drawback... the method only works if there are no multiple elements with the same name: if multiple elements are available, only the first one is searched. This is actually a bug in the method call interface itself, but it works for what it's needed, so...)
		*/
		string getPackageConfValue(ControlPackage *cp, string element, string attribute="");	// FIXME

		void sendFrame(ControlPackageConnection *connection, MediaCtrlFrame *frame);
		void incomingFrame(ControlPackageConnection *connection, MediaCtrlFrame *frame);
		void clearDtmfBuffer(ControlPackageConnection *connection);
		int getNextDtmfBuffer(ControlPackageConnection *connection);
		uint32_t getFlags(ControlPackageConnection *connection, int mediaType=MEDIACTRL_MEDIA_UNKNOWN);
		ControlPackageConnection *getSubConnection(ControlPackageConnection *connection, int mediaType);
		ControlPackageConnection *getSubConnection(ControlPackageConnection *connection, string label);

		/**
		* @fn decode(MediaCtrlFrame *frame);
		* A method to generically decode a frame: it wraps the call to the callback manager (i.e. the MediaCtrl core).
		* @param frame The frame to decode
		* @returns The decoded frame if successful, NULL otherwise
		*/
		MediaCtrlFrame *decode(MediaCtrlFrame *frame);
		/**
		* @fn encode(MediaCtrlFrame *frame, int dstFormat);
		* A method to generically encode a frame: it wraps the call to the callback manager (i.e. the MediaCtrl core).
		* @param frame The frame to encode
		* @param dstFormat The format to encode the frame to
		* @returns The encoded frame if successful, NULL otherwise
		*/
		MediaCtrlFrame *encode(MediaCtrlFrame *frame, int dstFormat);

	private:
		/**
		* @fn loadPackages();
		* Loads all the package plugins (*.so) from the packages folder
		*/
		void loadPackages();

		/**
		* @fn getPackage(string name)
		* Gets the ControlPackage instance identified by the provided name.
		* @param name The name of the package (e.g. "msc-ivr-basic")
		* @returns A pointer to the ControlPackage instance, if it exists, NULL otherwise
		*/
		ControlPackage *getPackage(string name);
		/**
		* @fn getSupportedPackages()
		* Gets the list of supported Control Packages as a string.
		* @returns A string containing the list of supported Control Packages
		*/
		string getSupportedPackages();
		/**
		* @fn getTransaction(string tid)
		* Gets the CfwTransaction instance identified by the provided identifier.
		* @param tid The transaction ID
		* @returns A pointer to the CfwTransaction instance, if it exists, NULL otherwise
		*/
		CfwTransaction *getTransaction(string tid);
		/**
		* @fn removeTransaction(string tid)
		* Removes and destroys the CfwTransaction instance identified by the provided identifier.
		* @param tid The transaction ID
		* @note This method is invoked only internally. Transactions disable themselves by means of the endedTransaction method
		*/
		void removeTransaction(string tid);

		/**
		* @fn endDialog(MediaCtrlClient *client)
		* Sets the specified client as no longer valid for ongoing transactions, since the session with the AS ended.
		* @param client The invalidated client
		* @note This method does not close the controlo channel connection itself: it just sets a flag stating that the dialog has ended. The connection is actually closed only subsequently by the CfwStack thread.
		*/
		void endDialog(MediaCtrlClient *client);
		list<string> endedDialogs;		/*!< List of no longer valid clients (callId associated to AS sessions) */

		/**
		* @fn connectionLost(int fd)
		* One of the MediaCtrlClient instances notified the lost connection
		* @param fd The affected file descriptor
		* @note This method only updated the file descriptors mapping with the MediaCtrlClient instance
		*/
		void connectionLost(int fd);
		/**
		* @fn parseMessage(MediaCtrlClient *client, string message)
		* Parses a received message, and acts accordingly.
		* @param client The client from where the message came
		* @param message The message header
		* @note This method only parses the header: the protocol behaviour is handled here, and so its here that the header is parsed according to the received protocol message. In case a payload is present according to the header, it's here that it is retrieved from the socket and handled (TODO: A better handling of incoming message should be implemented (e.g. a pool of per-AS buffers, which are parsed in per-AS threads)
		*/
		void parseMessage(MediaCtrlClient *client, string message);

		CfwManager* cfwManager;			/*!< The object handling CFW-related events, i.e. who to notify about them */

		list<MediaCtrlClient *> clients;		/*!< List of clients to the MediaCtrl framework */
		list<string> packagesString;			/*!< List of Control Packages (strings) */
		list<ControlPackage *> packages;		/*!< List of Control Packages (plugins) */
		list<ControlPackageFactory *>pkgFactories;	/*!< List of Control Packages (factories) */
		list<void *>pkgSharedObjects;			/*!< List of handles to the package shared objects */
		string supportedPackages;			/*!< List of supported Control Packages (string) */

		map<string, CfwTransaction *> transactions;	/*!< Map of currently handled transactions (tid) */
		list<string>endedTransactions;				/*!< List of ended transactions to free */
		ost::Mutex mTransactions, mEnded;

		map<int, MediaCtrlClient *> clientsMap;		/*!< Map of (fd,clients) */
		ost::Mutex mClients;

		bool alive;			/*!< Whether the stack is up and running or not */
		bool hardKeepAlive;	/*!< If TRUE, we send a BYE when a Keep-Alive timer timeouts (default=TRUE) */
		int server;			/*!< File descriptor */
		InetHostAddress address;	/*!< The public address */
		unsigned short int port;	/*!< Listening port */

		list<int> fds;			/*!< List of known file descriptors (clients) */
		struct pollfd pollfds[10];	/*!< Array of file descriptors to poll (clients) */
};

}

#endif
