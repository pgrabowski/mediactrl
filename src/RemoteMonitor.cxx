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
 * \brief Auditing Facility (telnet monitor)
 *
 * \author Lorenzo Miniero <lorenzo.miniero@unina.it>
 *
 * \ingroup utils
 */

#include "RemoteMonitor.h"

using namespace mediactrl;

RemoteMonitorRequest::RemoteMonitorRequest(int fd, string text)
{
	cout << "[RMT] New Remote Monitor request: " << text << " (fd=" << dec << fd << ")" << endl;
	this->request = text;
	this->fd = fd;
}

RemoteMonitorRequest::~RemoteMonitorRequest()
{
	// TODO
}

string RemoteMonitorRequest::getResponse()
{
	return response.str();	// FIXME
}


RemoteMonitor::RemoteMonitor()
{
	cout << "[RMT] Creating new Remote Monitor interface" << endl;
	alive = false;
	server = -1;
	port = 0;
	manager = NULL;
}

RemoteMonitor::~RemoteMonitor()
{
	cout << "[RMT] Destroying Remote Monitor interface" << endl;
	if(alive) {
		alive = false;
		join();
	}
}

int RemoteMonitor::setPort(uint16_t port)
{
	if(alive)	// Already listening
		return -1;
	this->port = port;

	address.sin_family = AF_INET;
	address.sin_port = htons(port);
	address.sin_addr.s_addr = INADDR_ANY;
	server = socket(AF_INET, SOCK_STREAM, 0);
	int yes = 1;
	if(setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) < 0)
		return -1;
	if(bind(server, (struct sockaddr *)(&address), sizeof(struct sockaddr)) < 0)
		return -1;
	listen(server, 5);
	cout << "[RMT] Remote Monitor interface listening on " << port << endl;
	fds.clear();
	pollfds[0].fd = server;
	pollfds[0].events = POLLIN;
	pollfds[0].revents = 0;
	fds.push_back(server);

	return 0;
}

void RemoteMonitor::setManager(RemoteMonitorManager *manager)
{
	this->manager = manager;	// FIXME
}

void RemoteMonitor::run()
{
	cout << "[RMT] Joining Remote Monitor interface thread" << endl;
	alive = true;
	socklen_t addrlen = sizeof(struct sockaddr);
	int err = 0, client = 0, len = 0, res = 0, fd = 0, trailer = 0;
	uint16_t i = 0;
	char byte, buffer[100];

	while(alive) {
		memset(buffer, 0, 100);
		while((err = poll(pollfds, fds.size(), 1000) < 0) && (errno == EINTR));
		if(err < 0)
			continue;	// Poll error, FIXME
		else {
			// Check which descriptor has data here ...
			i = 0;
			for(i = 0; i < fds.size(); i++) {
				if(err < 0)
					break;
				if(pollfds[i].revents == POLLIN) {
					err--;
					fd = pollfds[i].fd;
					if(fd == server) {	// We have a new connection from a client
						// Check if this is an expected client
						struct sockaddr_in client_address;
						client = accept(fd, (struct sockaddr *)(&client_address), &addrlen);
						if(client < 0)
							continue;
						cout << "[RMT] Accepted incoming Remote Monitor from " << inet_ntoa(client_address.sin_addr) << ":" << ntohs(client_address.sin_port) << endl;
						// Add client to list of known fds
						fds.push_back(client);
						pollfds[fds.size()-1].fd = client;
						pollfds[fds.size()-1].events = POLLIN;
					} else {	// There's data to read from a client
						len = 0;
						trailer = 0;
						while(1) {
							if(!alive)
								break;
							res = recv(fd, &byte, 1, 0);
							if(res < 1) {	// This client disconnected
								cout << "[RMT] Lost connection to fd=" << fd << endl;
								uint16_t i=0;
								for(i = 0; i <= fds.size(); i++) {
									if(pollfds[i].fd == fd) {
										pollfds[i].fd = -1;
										pollfds[i].events = 0;
										// FIXME
									}
								}
								len = -1;	// Failure
								break;
							} else {
								// Handle incoming msg
								memset(buffer+len, byte, 1);
								len++;
								switch(trailer) {
									case 1:
									case 3:
										if(byte == '\n')
											trailer++;
										else
											trailer = 0;
										break;
									case 0:
									case 2:
										if(byte == '\r')
											trailer++;
										else
											trailer = 0;
										break;
									default:
										trailer = 0;
										break;
								}
					//			if(strstr((const char *)buffer, "\r\n\r\n"))
					//				break;
								if(trailer == 4)	// Found the "\r\n\r\n"
									break;
							}
						}
						if(len < 1)
							continue;
						if(manager) {
							string text = buffer;
							string::size_type end = text.find("\r\n");
							RemoteMonitorRequest *request = new RemoteMonitorRequest(fd, text.substr(0, end));
							res = manager->remoteMonitorQuery(this, request);
							string response = "";
							if(res < 0) {
								response = "???\r\n";
							} else {
								response = request->getResponse();
							}
							send(fd, response.c_str(), response.length(), 0);
							delete request;
						}
					}
				}
			}
		}
	}

	cout << "[RMT] Leaving Remote Monitor interface thread" << endl;
}
