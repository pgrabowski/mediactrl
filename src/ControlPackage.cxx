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
 * \brief Connection Handler for Control Packages
 *
 * \author Lorenzo Miniero <lorenzo.miniero@unina.it>
 */

#include "ControlPackage.h"

using namespace mediactrl;

ControlPackageConnection::ControlPackageConnection(string Id, string label)
{
	this->Id = Id;
	this->label = label;
	connectionId = Id;
//	if(label != "")
//		connectionId += "~" + label;
	cout << "[CPCN] ControlPackageConnection: " << connectionId << endl;
	packages.clear();
//	mPackages.init();
	endpoint = NULL;
	pt = -1;
	type = CPC_CONNECTION;			// The default value
	mediaType = MEDIACTRL_MEDIA_AUDIO;	// The default value
}

ControlPackageConnection::~ControlPackageConnection()
{
	// TODO Reimplement automatic closing, which is broken now...
	connectionClosing(this, this);	// FIXME
	cout << "[CPCN] Removing ControlPackageConnection: " << connectionId << "..." << endl;
//	mPackages.enter();
//	while(!packages.empty());
//	mPackages.leave();
	cout << "[CPCN] \tDone ControlPackageConnection: " << connectionId << endl;
}

void ControlPackageConnection::addPackage(ControlPackage *cp)
{
	mPackages.enter();
	packages.push_back(cp);
	packages.sort();
	packages.unique();
	mPackages.leave();
}

void ControlPackageConnection::removePackage(ControlPackage *cp)
{
	mPackages.enter();
	packages.remove(cp);
	mPackages.leave();
}

void ControlPackageConnection::sendFrame(MediaCtrlFrame *frame)
{
	mPackages.enter();
	if((frame == NULL) || packages.empty()) {
		mPackages.leave();
		return;
	}
	list<ControlPackage *>::iterator iter;
	ControlPackage *pkg = NULL;
	for(iter = packages.begin(); iter != packages.end(); iter++) {
		pkg = (*iter);
		if(pkg != NULL)
			pkg->sendFrame(this, frame);
	}
	mPackages.leave();
}

void ControlPackageConnection::incomingFrame(ControlPackageConnection *connection, ControlPackageConnection *subConnection, MediaCtrlFrame *frame)
{
	mPackages.enter();
	if((frame == NULL) || packages.empty()) {
		mPackages.leave();
		return;
	}
	list<ControlPackage *>::iterator iter;
	ControlPackage *pkg = NULL;
	for(iter = packages.begin(); iter != packages.end(); iter++) {
		pkg = (*iter);
		if(pkg != NULL)
			pkg->incomingFrame(connection, subConnection, frame);
	}
	mPackages.leave();
}

void ControlPackageConnection::frameSent(ControlPackageConnection *connection, ControlPackageConnection *subConnection, MediaCtrlFrame *frame)
{
	mPackages.enter();
	if((frame == NULL) || packages.empty()) {
		mPackages.leave();
		return;
	}
	list<ControlPackage *>::iterator iter;
	ControlPackage *pkg = NULL;
	for(iter = packages.begin(); iter != packages.end(); iter++) {
		pkg = (*iter);
		if(pkg != NULL)
			pkg->frameSent(connection, subConnection, frame);
	}
	mPackages.leave();
}

void ControlPackageConnection::incomingDtmf(ControlPackageConnection *connection, ControlPackageConnection *subConnection, int type)
{
	mPackages.enter();
	if(packages.empty()) {
		mPackages.leave();
		return;
	}
	list<ControlPackage *>::iterator iter;
	ControlPackage *pkg = NULL;
	for(iter = packages.begin(); iter != packages.end(); iter++) {
		pkg = (*iter);
		if(pkg != NULL)
			pkg->incomingDtmf(connection, subConnection, type);
	}
	mPackages.leave();
}

void ControlPackageConnection::connectionLocked(ControlPackageConnection *connection, ControlPackageConnection *subConnection)
{
	mPackages.enter();
	if(packages.empty()) {
		mPackages.leave();
		return;
	}
	list<ControlPackage *>::iterator iter;
	ControlPackage *pkg = NULL;
	for(iter = packages.begin(); iter != packages.end(); iter++) {
		pkg = (*iter);
		if(pkg != NULL)
			pkg->connectionLocked(connection, subConnection);
	}
	mPackages.leave();
}

void ControlPackageConnection::connectionUnlocked(ControlPackageConnection *connection, ControlPackageConnection *subConnection)
{
	mPackages.enter();
	if(packages.empty()) {
		mPackages.leave();
		return;
	}
	list<ControlPackage *>::iterator iter;
	ControlPackage *pkg = NULL;
	for(iter = packages.begin(); iter != packages.end(); iter++) {
		pkg = (*iter);
		if(pkg != NULL)
			pkg->connectionUnlocked(connection, subConnection);
	}
	mPackages.leave();
}

void ControlPackageConnection::connectionClosing(ControlPackageConnection *connection, ControlPackageConnection *subConnection)
{
	mPackages.enter();
	if(packages.empty()) {
		mPackages.leave();
		return;
	}
	ControlPackages tmp = packages;
	packages.clear();	// FIXME
	mPackages.leave();
	list<ControlPackage *>::iterator iter;
	ControlPackage *pkg = NULL;
	for(iter = tmp.begin(); iter != tmp.end(); iter++) {
		pkg = (*iter);
		if(pkg != NULL)
			pkg->connectionClosing(connection, subConnection);
	}
}
