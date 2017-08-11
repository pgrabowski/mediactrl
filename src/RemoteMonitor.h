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

#ifndef _REMOTE_MONITOR_H
#define _REMOTE_MONITOR_H

/*! \file
 *
 * \brief Headers: Auditing Facility (telnet monitor)
 *
 * \author Lorenzo Miniero <lorenzo.miniero@unina.it>
 *
 * \ingroup utils
 * \ref utils
 */


#include <sys/time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <time.h>

#include <sys/types.h>
#include <sstream>
#include <iostream>
#include <memory>
#include <list>

#include <cc++/thread.h>

#include "MediaCtrlMemory.h"

using namespace std;
using namespace ost;

namespace mediactrl {

class RemoteMonitor;
class RemoteMonitorRequest;

/// Auditing Requests Callback
/**
* @class RemoteMonitorManager RemoteMonitor.h
* An abstract class implemented by the core MediaCtrl object in order to receive callback notifications related to auditing requests.
*/
class RemoteMonitorManager {
	public:
		RemoteMonitorManager() {};
		virtual ~RemoteMonitorManager() {};

		virtual int remoteMonitorQuery(RemoteMonitor *monitor, RemoteMonitorRequest *request) = 0;
};

/// Auditing Request
/**
* @class RemoteMonitorRequest RemoteMonitor.h
* A helper class to handle incoming auditing requests.
*/
class RemoteMonitorRequest : public gc {
	public:
		/**
		* @fn RemoteMonitorRequest(int fd, string text);
		* Constructor. Creates a new RemoteMonitorRequest instance out of an incoming auditing request.
		* @param fd The file descriptor from where the auditing request came from
		* @param text The content of the request
		* @note The same file descriptor (fd) is used when aswering to the request... of course this assumes the RemoteMonitor
		* which made the request did not disconnect, and was not replaced by another one with the same fd... probably to be fixed.
		*/
		RemoteMonitorRequest(int fd, string text);

		/**
		* @fn ~RemoteMonitorRequest();
		* Destructor.
		*/
		~RemoteMonitorRequest();

		/**
		* @fn int getFd();
		* Return the file descriptor associated with the requester.
		* @returns fd (check the previous note)
		*/
		int getFd() { return fd; };
		/**
		* @fn getRequest();
		* Get the content of the request.
		* @returns The content of the original request.
		*/
		string getRequest() { return request; };
		/**
		* @fn getResponse();
		* Get the response to the auditing request which originated the object.
		* @returns The response if available, an empty string otherwise
		*/
		string getResponse();

		/**
		* @fn addToResponse();
		* Append text to the response: this should only be used by the entity answering to the request.
		* @note This method works like the << operator (e.g. request.addToResponse() << "New text\r\n"). The "\r\n" must be used in place of endl.
		*/
		stringstream *addToResponse() { return &response; };	// FIXME

	private:
		int fd;			/*!< The file descriptor from where the request came out */
		string request;		/*!< The original auditing request */
		stringstream response;	/*!< The stringstream buffer containing the response while it is constructed */
};

/// Auditing Facility
/**
* @class RemoteMonitor RemoteMonitor.h
* A class implementing an auditing interface.
* @note The RemoteMonitor interface is currently implemented as a telnet-like server: simple text commands can be sent to the server to receive information (e.g. "sip", "cfw all", etc.). It's just a debugging feature, NOT a standardized auditing interface for the framework: so far no efforts have been put in this by the WG, even though this will be done in the future.
*/
class RemoteMonitor : public gc, public Thread {
	public:
		RemoteMonitor();
		~RemoteMonitor();

		/**
		* @fn setPort(uint16_t port);
		* Specifies the listening port for this interface, binds to it and waits for incomng connections
		* @param port The port to listen on
		* @returns 0 on success, -1 otherwise
		*/
		int setPort(uint16_t port);
		/**
		* @fn setManager(RemoteMonitorManager *manager);
		* Specifies which object will have to receive callback event notifications about requests.
		* @param manager The object to notify
		*/
		void setManager(RemoteMonitorManager *manager);

	private:
		bool alive;			/*!< Whether this monitor is active/alive or not */
		/**
		* @fn run();
		* The thread handling incoming requests on this interface.
		*/
		void run();

		int server;			/*!< The file descriptor associated with this listening interface */
		RemoteMonitorManager *manager;	/*!< The object to notify about incoming requests */
		uint16_t port;			/*!< The port the interface is listening on */
		struct sockaddr_in address;	/*!< The address of the server interface */

		list<int> fds;			/*!< List of known file descriptors (clients) */
		struct pollfd pollfds[10];	/*!< Array of file descriptors to poll (clients) */

		// TODO Add authentication
};

}

#endif
