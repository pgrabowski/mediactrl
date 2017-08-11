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
 * \brief Wrapper for Handled Endpoints (connections+conferences)
 *
 * \author Lorenzo Miniero <lorenzo.miniero@unina.it>
 *
 * \ingroup core
 * \ref core
 */

#include "MediaCtrlEndpoint.h"

using namespace mediactrl;

// Connection
MediaCtrlConnection::MediaCtrlConnection(string connectionId, string label)
{
	media = MEDIACTRL_MEDIA_UNKNOWN;	// We don't know yet (and might never known, if this is a wrapper)
	type = MEDIACTRL_CONNECTION;
	Id = connectionId;
	this->label = label;
	cout << "[ENDP] Created new connection: " << connectionId;
	if(label != "")
		cout << "~" << label;
	cout << endl;
	sipTransaction = NULL;
	rtpChannel = NULL;
	// Create a new abstract connection and attach it to the endpoint
	cpConnection = new ControlPackageConnection(connectionId, label);	// FIXME
	cpConnection->setType(CPC_CONNECTION);
	cpConnection->setMediaType(MEDIACTRL_MEDIA_UNKNOWN);
	setCpConnection(cpConnection);
	cpConnection->setEndpoint(this);
	channels.clear();
	owner = NULL;
	counter = 0;
}

MediaCtrlConnection::~MediaCtrlConnection()
{
	// FIXME
	cout << "[ENDP] Destroying connection: " << Id;
	if(label != "")
		cout << "~" << label;
	cout << endl;
	if(cpConnection == NULL)
		return;
//	cpConnection->connectionClosing();
	delete cpConnection;
	if(!channels.empty()) {
		MediaCtrlConnection *c = NULL;
		while(!channels.empty()) {
			c = channels.front();
			channels.pop_front();
			if(c)
				delete c;
		}
	}

}

void MediaCtrlConnection::setLocal(MediaCtrlSipTransaction *sipTransaction, MediaCtrlRtpChannel *rtpChannel)
{
	cout << "[ENDP] Setting SIP and RTP callbacks: " << Id;
	if(label != "")
		cout << "~" << label;
	cout << endl;
	this->sipTransaction = sipTransaction;
	this->rtpChannel = rtpChannel;
	media = rtpChannel->getMediaType();
	cpConnection->setPayloadType(rtpChannel->getPayloadType());
	cpConnection->setMediaType(media);
}

int MediaCtrlConnection::getFormat(int mediaType)
{
	if(rtpChannel != NULL) {
		if((mediaType == MEDIACTRL_MEDIA_UNKNOWN) || (mediaType == media))
			return rtpChannel->getPayloadType();
	}
	else if(!channels.empty()) {
		if(channels.size() == 1)
			return channels.front()->getFormat(mediaType);	// FIXME FIXME FIXME
		list<MediaCtrlConnection*>::iterator iter;
		for(iter = channels.begin(); iter != channels.end(); iter++) {
			if((*iter)->getMediaType() == mediaType)
				return (*iter)->getFormat(mediaType);
		}
	}
	return -1;
}

uint32_t MediaCtrlConnection::getFlags(int mediaType)
{
	if(rtpChannel != NULL) {
		if((mediaType == MEDIACTRL_MEDIA_UNKNOWN) || (mediaType == media))
			return rtpChannel->getFlags();
	} else if(!channels.empty()) {
//		if(mediaType == MEDIACTRL_MEDIA_UNKNOWN)
//			return channels.front()->getFlags();
		list<MediaCtrlConnection*>::iterator iter;
		for(iter = channels.begin(); iter != channels.end(); iter++) {
			if((*iter)->getMediaType() == mediaType)
				return (*iter)->getFlags();
		}
	}
	return 0;	// Fallback
}

void MediaCtrlConnection::clearDtmfBuffer()
{
	if(rtpChannel != NULL)
		rtpChannel->clearDtmfBuffer();
	else if(!channels.empty()) {
		list<MediaCtrlConnection*>::iterator iter;
		for(iter = channels.begin(); iter != channels.end(); iter++)
			(*iter)->clearDtmfBuffer();
	}
}

int MediaCtrlConnection::getNextDtmfBuffer()
{
	if(rtpChannel != NULL)
		return rtpChannel->getNextDtmfBuffer();
	else if(!channels.empty()) {
		list<MediaCtrlConnection*>::iterator iter;
		for(iter = channels.begin(); iter != channels.end(); iter++) {
			if((*iter)->getMediaType() == MEDIACTRL_MEDIA_AUDIO)
				return (*iter)->getNextDtmfBuffer();
		}
	}
	return -1;
}

void MediaCtrlConnection::sendFrame(MediaCtrlFrame *frame)
{
	if(frame == NULL)
		return;
	if((rtpChannel != NULL) && (rtpChannel->getMediaType() == frame->getMediaType()))
		rtpChannel->sendFrame(frame);
	else if(!channels.empty()) {
		list<MediaCtrlConnection*>::iterator iter;
		for(iter = channels.begin(); iter != channels.end(); iter++) {
			if((*iter)->getMediaType() == frame->getMediaType())
				(*iter)->sendFrame(frame);
		}
	}
}

void MediaCtrlConnection::payloadTypeChanged(MediaCtrlSipTransaction *sipTransaction, MediaCtrlRtpChannel *rtpChannel, int pt)
{
	if(cpConnection != NULL)
		cpConnection->setPayloadType(pt);
}

void MediaCtrlConnection::incomingFrame(MediaCtrlSipTransaction *sipTransaction, MediaCtrlRtpChannel *rtpChannel, MediaCtrlFrame *frame)
{
	// This method is only called on subconnections (the ones associated with an RTP channel)
	if((cpConnection != NULL) && (owner != NULL) && (rtpChannel != NULL)) {
		cpConnection->incomingFrame(owner->getCpConnection(), cpConnection, frame);
		owner->incomingFrame(cpConnection, frame);
	}
}

void MediaCtrlConnection::incomingFrame(ControlPackageConnection *subConnection, MediaCtrlFrame *frame)
{
	// This method is only called on abstract connections by their subconnections
	if(cpConnection != NULL)
		cpConnection->incomingFrame(cpConnection, subConnection, frame);
}

void MediaCtrlConnection::incomingDtmf(MediaCtrlSipTransaction *sipTransaction, MediaCtrlRtpChannel *rtpChannel, int type)
{
	// This method is only called on subconnections (the ones associated with an RTP channel)
	if((cpConnection != NULL) && (owner != NULL) && (rtpChannel != NULL)) {
		cpConnection->incomingDtmf(owner->getCpConnection(), cpConnection, type);
		owner->incomingDtmf(cpConnection, type);
	}
}

void MediaCtrlConnection::incomingDtmf(ControlPackageConnection *subConnection, int type)
{
	// This method is only called on abstract connections by their subconnections
	if(cpConnection != NULL)
		cpConnection->incomingDtmf(cpConnection, subConnection, type);
}

void MediaCtrlConnection::frameSent(MediaCtrlSipTransaction *sipTransaction, MediaCtrlRtpChannel *rtpChannel, MediaCtrlFrame *frame)
{
	// This method is only called on subconnections (the ones associated with an RTP channel)
	if((cpConnection != NULL) && (owner != NULL) && (rtpChannel != NULL)) {
		cpConnection->frameSent(owner->getCpConnection(), cpConnection, frame);
		owner->frameSent(cpConnection, frame);
	}
}

void MediaCtrlConnection::frameSent(ControlPackageConnection *subConnection, MediaCtrlFrame *frame)
{
	// This method is only called on abstract connections by their subconnections
	if(cpConnection != NULL)
		cpConnection->frameSent(cpConnection, subConnection, frame);
}

void MediaCtrlConnection::channelLocked(MediaCtrlSipTransaction *sipTransaction, MediaCtrlRtpChannel *rtpChannel)
{
	// This method is only called on subconnections (the ones associated with an RTP channel)
	if((cpConnection != NULL) && (owner != NULL) && (rtpChannel != NULL)) {
		cpConnection->connectionLocked(owner->getCpConnection(), cpConnection);
		owner->channelLocked(cpConnection);
	}
}

void MediaCtrlConnection::channelLocked(ControlPackageConnection *subConnection)
{
	// This method is only called on abstract connections by their subconnections
	if(cpConnection != NULL)
		cpConnection->connectionLocked(cpConnection, subConnection);
}

void MediaCtrlConnection::channelUnlocked(MediaCtrlSipTransaction *sipTransaction, MediaCtrlRtpChannel *rtpChannel)
{
	// This method is only called on subconnections (the ones associated with an RTP channel)
	if((cpConnection != NULL) && (owner != NULL) && (rtpChannel != NULL)) {
		cpConnection->connectionUnlocked(owner->getCpConnection(), cpConnection);
		owner->channelUnlocked(cpConnection);
	}
}

void MediaCtrlConnection::channelUnlocked(ControlPackageConnection *subConnection)
{
	// This method is only called on abstract connections by their subconnections
	if(cpConnection != NULL)
		cpConnection->connectionUnlocked(cpConnection, subConnection);
}

void MediaCtrlConnection::channelClosed(string connectionId, string label)
{
	// This method is only called on abstract connections by their subconnections
	cout << "[ENDP] Channel closing: " << connectionId << " / " << label << endl;
	if((cpConnection != NULL) && (owner != NULL) && (rtpChannel != NULL)) {
		cpConnection->connectionClosing(owner->getCpConnection(), cpConnection);
		owner->channelClosed(cpConnection, connectionId);
	}
}

void MediaCtrlConnection::channelClosed(ControlPackageConnection *subConnection, string connectionId)
{
	// This method is only called on abstract connections by their subconnections
	cout << "[ENDP] Channel closing (wrapper): " << connectionId << " / " << endl;
	if(cpConnection != NULL)
		cpConnection->connectionClosing(cpConnection, subConnection);
}

int MediaCtrlConnection::addOwned(MediaCtrlConnection *connection)
{
	if(label != "")		// We're not a wrapper
		return -1;
	if(!connection || (connection->getLabel() == ""))
		return -1;
	channels.push_back(connection);
	return 0;
}

int MediaCtrlConnection::removeOwned(MediaCtrlConnection *connection)
{
	if(label != "")		// We're not a wrapper
		return -1;
	if(!connection || (connection->getLabel() == ""))
		return -1;
	channels.remove(connection);
	return 0;
}

MediaCtrlConnection *MediaCtrlConnection::getOwned(int mediaType)
{
	if(mediaType != MEDIACTRL_MEDIA_AUDIO)
		return NULL;
	if(channels.empty())
		return NULL;
	list<MediaCtrlConnection*>::iterator iter;
	for(iter = channels.begin(); iter != channels.end(); iter++) {
		MediaCtrlConnection *c = (*iter);
		if(c->media == mediaType)
			return c;
	}
	return NULL;
}

MediaCtrlConnection *MediaCtrlConnection::getOwned(string ownedLabel)
{
	if(label != "")
		return NULL;
	if(channels.empty())
		return NULL;
	list<MediaCtrlConnection*>::iterator iter;
	for(iter = channels.begin(); iter != channels.end(); iter++) {
		MediaCtrlConnection *c = (*iter);
		cout << "[ENDP] Comparing labels: " << ownedLabel << " ?= " << c->getLabel() << endl;
		if(c->label == ownedLabel)
			return c;
	}
	return NULL;
}

int MediaCtrlConnection::setOwner(MediaCtrlConnection *connection)
{
	if(label == "")		// We can not be owned
		return -1;
	if(!connection || (connection->getLabel() != ""))
		return -1;
	owner = connection;
	return 0;
}

MediaCtrlConnection *MediaCtrlConnection::getOwner()
{
	if(label == "")
		return NULL;
	return owner;
}

void MediaCtrlConnection::increaseCounter()
{
	counter++;
	if(owner && (label != ""))
		rtpChannel->wakeUp(true);
	else if(!channels.empty() && (label == "")) {
		list<MediaCtrlConnection*>::iterator iter;
		for(iter = channels.begin(); iter != channels.end(); iter++)
			(*iter)->increaseCounter();
	}
}

void MediaCtrlConnection::decreaseCounter()
{
	if(counter > 0)
	counter--;
	if(owner && (label != "") && (counter == 0))
		rtpChannel->wakeUp(false);
	else if(!channels.empty() && (label == "")) {
		list<MediaCtrlConnection*>::iterator iter;
		for(iter = channels.begin(); iter != channels.end(); iter++)
			(*iter)->decreaseCounter();
	}
}


// Conference
MediaCtrlConference::MediaCtrlConference(string confId, string label)
{
	media = MEDIACTRL_MEDIA_UNKNOWN;	// This endpoint wraps many heterogeneous connections
	type = MEDIACTRL_CONFERENCE;
	Id = confId;
	label = "";	// Unused in conferences
	cout << "[ENDP] Created new conference: " << confId << endl;
	// Create a new abstract conference connection and attach it to the endpoint
	cpConnection = new ControlPackageConnection(confId);
	cpConnection->setType(CPC_CONFERENCE);
	cpConnection->setMediaType(MEDIACTRL_MEDIA_UNKNOWN);
	setCpConnection(cpConnection);
	cpConnection->setEndpoint(this);
}

MediaCtrlConference::~MediaCtrlConference()
{
	cout << "[ENDP] Destroying conference: " << Id << endl;
	if(cpConnection == NULL)
		return;
//	cpConnection->connectionClosing();
	delete cpConnection;
	// TODO better handling
}

void MediaCtrlConference::sendFrame(MediaCtrlFrame *frame)
{
	if(cpConnection == NULL)
		return;
	cpConnection->sendFrame(frame);
}

void MediaCtrlConference::incomingFrame(MediaCtrlFrame *frame)
{
	if(cpConnection == NULL)
		return;
	cpConnection->incomingFrame(cpConnection, cpConnection, frame);
}

uint32_t MediaCtrlConference::getFlags(int mediaType)
{
	return 0;
}

