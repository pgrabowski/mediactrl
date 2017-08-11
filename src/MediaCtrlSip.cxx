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
 * \brief Session Initiation Protocol (SIP) Transactions Handler
 *
 * \author Lorenzo Miniero <lorenzo.miniero@unina.it>
 *
 * \ingroup core
 * \ref core
 */


#include "MediaCtrlSip.h"

using namespace mediactrl;


MediaCtrlSipTransaction::MediaCtrlSipTransaction(string callId, ServerInviteSessionHandle sis)
{
	cout << "[SIP] Creating SIP transaction " << callId << endl;
	this->callId = callId;
	this->sis = sis;
	as = false;	// By default, a new SIP transaction is not related to Application Servers
	negotiated = false;

	mLinks = new ost::Mutex();
	active = false;
}

MediaCtrlSipTransaction::~MediaCtrlSipTransaction()
{
	mLinks->enter();

	active = false;

	cout << "[SIP] Removing SIP transaction " << callId << " ..." << endl;
	list<uint16_t>::iterator iter;
	MediaCtrlRtpChannel *rtp;
	delete mLinks;
	if(rtpPorts.empty())
		return;
	sipManagers.clear();
	for(iter = rtpPorts.begin(); iter != rtpPorts.end(); iter++ ) {
		rtp = rtpConnectionsByPort[(*iter)];
		if(rtp) {
//			rtpConnectionsByPort[(*iter)] = NULL;
			rtpConnectionsByPort.erase(rtp->getSrcPort());
//			rtpConnectionsByLabel[rtp->getLabel()] = NULL;
			rtpConnectionsByLabel.erase(rtp->getLabel());
			delete rtp;
		}
	}
	mLinks->leave();
	cout << "[SIP] SIP transaction (" << callId << ") removed " << endl;
}

bool MediaCtrlSipTransaction::rtpExists(string ip, uint16_t port)
{
	list<uint16_t>::iterator iter;
	MediaCtrlRtpChannel *rtp;
	for(iter = rtpPorts.begin(); iter != rtpPorts.end(); iter++ ) {
		rtp = rtpConnectionsByPort[(*iter)];
		if(rtp) {
			if((ip == rtp->getDstIp()) && (port == rtp->getDstPort()))
				return true;
		}
	}
	return false;
}

uint16_t MediaCtrlSipTransaction::addRtp(int pt, int media)
{
	MediaCtrlRtpChannel *rtp = new MediaCtrlRtpChannel(address, media);	// FIXME
	uint16_t newport = rtp->getSrcPort();
	rtpPorts.push_back(newport);
	rtpLabels.push_back(rtp->getLabel());
	rtpConnectionsByPort[newport] = rtp;
	rtpConnectionsByLabel[rtp->getLabel()] = rtp;
	rtp->setManager(this);
	rtp->setPayloadType(pt);

	return newport;
}

bool MediaCtrlSipTransaction::setRtpPeer(uint16_t localPort, const InetHostAddress &ia, uint16_t dataPort)
{
	MediaCtrlRtpChannel *rtp = rtpConnectionsByPort[localPort];
	if(!rtp)
		return false;

	return rtp->setPeer(ia, dataPort);
}

bool MediaCtrlSipTransaction::setRtpDirection(uint16_t localPort, int direction)
{
	MediaCtrlRtpChannel *rtp = rtpConnectionsByPort[localPort];
	if(!rtp)
		return false;

	return rtp->setDirection(direction);
}

string MediaCtrlSipTransaction::addRtpSetting(uint16_t localPort, string value)
{
	MediaCtrlRtpChannel *rtp = rtpConnectionsByPort[localPort];
	if(!rtp)
		return "";

	return rtp->addSetting(value);
}

void MediaCtrlSipTransaction::setTags(string fromTag, string toTag)
{
	this->fromTag = fromTag;
	this->toTag = toTag;
	connectionId = fromTag + "~" + toTag;
}

string MediaCtrlSipTransaction::getFromTag()
{
	return fromTag;
}

string MediaCtrlSipTransaction::getToTag()
{
	return toTag;
}

string MediaCtrlSipTransaction::getConnectionId(uint16_t localPort)
{
	if(localPort == 0)	// Get the main connection (usually audio) FIXME
		return connectionId;

	MediaCtrlRtpChannel *rtp = rtpConnectionsByPort[localPort];
	if(!rtp)
		return "";
	string label = connectionId + "/" + rtp->getLabel();
	return label;
}

string MediaCtrlSipTransaction::getConnectionId(string label)
{
	MediaCtrlRtpChannel *rtp = rtpConnectionsByLabel[label];
	if(!rtp)
		return "";
	string connlabel = connectionId + "/" + rtp->getLabel();
	return connlabel;
}

string MediaCtrlSipTransaction::getMediaLabel(uint16_t localPort)
{
	MediaCtrlRtpChannel *rtp = rtpConnectionsByPort[localPort];
	if(!rtp)
		return "";
	return rtp->getLabel();
}

uint16_t MediaCtrlSipTransaction::getMediaPort(string label)
{
	MediaCtrlRtpChannel *rtp = rtpConnectionsByLabel[label];
	if(!rtp)
		return 0;
	return rtp->getSrcPort();
}

MediaCtrlRtpChannel *MediaCtrlSipTransaction::getRtpChannel(string label)
{
	if(label == "")
		label = rtpLabels.front();
	return rtpConnectionsByLabel[label];
}

int MediaCtrlSipTransaction::setSipManager(MediaCtrlSipManager *manager, string label)
{
	mLinks->enter();

	if(!active)
		active = true;

	if(label != "") {	// A label has been specified, take a specific media
		if(rtpConnectionsByLabel[label] == NULL) {
			mLinks->leave();
			return -1;
		}
		sipManagers[label].push_front(manager);
		singleManager = false;
	} else {		// Take all the media managed by this Dialog
		singleManager = true;
		map<string, MediaCtrlRtpChannel *>::iterator iter;
		for(iter = rtpConnectionsByLabel.begin(); iter != rtpConnectionsByLabel.end(); iter++) {
			sipManagers[iter->first].push_front(manager);
		}
	}
	mLinks->leave();
	return 0;
}

int MediaCtrlSipTransaction::unsetSipManager(MediaCtrlSipManager *manager, string label)
{
	mLinks->enter();
	if(manager == NULL) {
		sipManagers.clear();
		map<string, MediaCtrlRtpChannel *>::iterator iter;
		for(iter = rtpConnectionsByLabel.begin(); iter != rtpConnectionsByLabel.end(); iter++)
			iter->second->setManager(NULL);
		mLinks->leave();
		return 0;
	}
	if(label != "") {
		if(rtpConnectionsByLabel[label] == NULL) {
			mLinks->leave();
			return -1;
		}
		sipManagers[label].clear();
//		sipManagers[label].remove(manager);
	} else {		// All the media managed by this Dialog
		map<string, MediaCtrlRtpChannel *>::iterator iter;
		for(iter = rtpConnectionsByLabel.begin(); iter != rtpConnectionsByLabel.end(); iter++)
			sipManagers[iter->first].clear();
//			sipManagers[iter->first].remove(manager);
	}
	mLinks->leave();

	return 0;
}

void MediaCtrlSipTransaction::payloadTypeChanged(MediaCtrlRtpChannel *rtpChannel, int pt)
{
	mLinks->enter();
	if(!active) {
		mLinks->leave();
		return;
	}
	if(!sipManagers[rtpChannel->getLabel()].empty())
		sipManagers[rtpChannel->getLabel()].front()->payloadTypeChanged(this, rtpChannel, pt);
	mLinks->leave();
}

void MediaCtrlSipTransaction::incomingFrame(MediaCtrlRtpChannel *rtpChannel, MediaCtrlFrame *frame)
{
	mLinks->enter();
	if(!active) {
		mLinks->leave();
		return;
	}
	if(!sipManagers[rtpChannel->getLabel()].empty())
		sipManagers[rtpChannel->getLabel()].front()->incomingFrame(this, rtpChannel, frame);
	mLinks->leave();
}

void MediaCtrlSipTransaction::incomingDtmf(MediaCtrlRtpChannel *rtpChannel, int type)
{
	mLinks->enter();
	if(!active) {
		mLinks->leave();
		return;
	}
	if(!sipManagers[rtpChannel->getLabel()].empty())
		sipManagers[rtpChannel->getLabel()].front()->incomingDtmf(this, rtpChannel, type);
	mLinks->leave();
}

void MediaCtrlSipTransaction::frameSent(MediaCtrlRtpChannel *rtpChannel, MediaCtrlFrame *frame)
{
	mLinks->enter();
	if(!active) {
		mLinks->leave();
		return;
	}
	if(!sipManagers[rtpChannel->getLabel()].empty())
		sipManagers[rtpChannel->getLabel()].front()->frameSent(this, rtpChannel, frame);
	mLinks->leave();
}

void MediaCtrlSipTransaction::channelLocked(MediaCtrlRtpChannel *rtpChannel)
{
	mLinks->enter();
	if(!active) {
		mLinks->leave();
		return;
	}
	if(!sipManagers[rtpChannel->getLabel()].empty())
		sipManagers[rtpChannel->getLabel()].front()->channelLocked(this, rtpChannel);
	mLinks->leave();
}

void MediaCtrlSipTransaction::channelUnlocked(MediaCtrlRtpChannel *rtpChannel)
{
	mLinks->enter();
	if(!active) {
		mLinks->leave();
		return;
	}
	if(!sipManagers[rtpChannel->getLabel()].empty())
		sipManagers[rtpChannel->getLabel()].front()->channelUnlocked(this, rtpChannel);
	mLinks->leave();
}

void MediaCtrlSipTransaction::channelClosed(string label)
{
	mLinks->enter();
	active = false;

	if(!sipManagers[label].empty()) {
		MediaCtrlSipManager *closingManager = sipManagers[label].front();
		sipManagers[label].clear();
		if(closingManager != NULL)
			sipManagers[label].front()->channelClosed(connectionId, label);
	}
	MediaCtrlRtpChannel *rtp = rtpConnectionsByLabel[label];
	rtpConnectionsByLabel.erase(label);
	rtpLabels.remove(label);
	if(rtp) {
		uint16_t portNum = rtp->getSrcPort();
		rtpConnectionsByPort.erase(portNum);
		rtpPorts.remove(portNum);
	}
	mLinks->leave();
}
