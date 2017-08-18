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
 * \brief CFW Protocol Stack and Handler (draft-ietf-mediactrl-sip-control-framework-10)
 *
 * \author Lorenzo Miniero <lorenzo.miniero@unina.it>
 *
 * \ingroup core
 * \ref core
 */

#include "CfwStack.h"
#include <dlfcn.h>
#include <dirent.h>
#include <sstream>
#include <boost/regex.hpp>

using namespace mediactrl;
using namespace boost;


/// Status defines for Control Framework messages
enum {
	/*! 200 */
	CFW_200 = 0,
	/*! 202 (extend transaction and restart timer) */
	CFW_202,
	/*! 403 (forbidden, whenever a cfw-id is not authorized) */
	CFW_403,
	/*! update (refresh a 202 and restart timer) */
	CFW_REPORT_UPDATE,
	/*! terminate (ends the transaction as well) */
	CFW_REPORT_TERMINATE,
};


/// Trivial helper to shutdown connections (shutdown is overloaded and can't be used...)
void shutdownServer(int fd);
void shutdownServer(int fd)
{
	shutdown(fd, SHUT_RDWR);
	close(fd);
}


/// Random string generator, for transactions originated by the framework (CONTROL notifications)
string random_tid(void);
string random_tid()
{
	size_t size = 13;
	long val[4];
	int x;

	for (x = 0; x < 4; x++)
		val[x] = random();
	char buf[size];
	memset(buf, 0, size);
	snprintf(buf, size, "%08lx%08lx%08lx%08lx", val[0], val[1], val[2], val[3]);

	string label = buf;
	return label;
}


// CfwMessage: a message of the transaction
CfwMessage::CfwMessage(int seq, string text)
{
	cout << "[CFW] Creating message: seq=" << dec << seq << endl;
	this->seq = seq;
	this->text = text;
	waitingForAck = true;
	sent = false;
}

CfwMessage::~CfwMessage()
{
	cout << "[CFW] Erasing message: seq=" << dec << seq << endl;
}


// The transaction handler
CfwTransaction::CfwTransaction(CfwStack *cfw, MediaCtrlClient *client, string tid)
{
	cout << "[CFW] New transaction: " << tid << endl;
	this->cfw = cfw;
	this->client = client;
	this->tid = tid;
	seq = 0;
	status = -1;
	active = running = true;
	messages.clear();
	oldMessages.clear();
	timeoutTimer = NULL;
	timeoutStart = 0;
	start();
}

CfwTransaction::~CfwTransaction()
{
	cout << "[CFW] Destroying transaction: " << tid << endl;
	if(running) {
		running = false;
		join();
	}
	if(timeoutTimer != NULL)
		delete timeoutTimer;
	cout << "[CFW] \t\tTransaction destroyed" << endl;
}

void CfwTransaction::queueMessage(CfwMessage *msg, int newstatus)
{
	if(!msg)
		return;
	if(status == CFW_REPORT_TERMINATE)
		return;

	cout << "[CFW] Queueing message: seq=" << dec << msg->getSeq() << endl;

	mMsg.enter();
	messages.push_back(msg);
	if(newstatus != -1)
		status = newstatus;
//	if((status == CFW_202) || (status == CFW_REPORT_UPDATE))	// An update/202 has been triggered, do we need to create a transaction timeout timer?
//		restartTimer();
	mMsg.leave();

	cout << "[CFW] Queued message: seq=" << dec << msg->getSeq() << endl;
}

void CfwTransaction::startTimer()
{
	if(timeoutTimer == NULL) {
		timeoutTimer = new TimerPort();
		timeoutTimer->incTimer(0);	// Start the timer and initialize the reference start time
	}
	timeoutStart = timeoutTimer->getElapsed();
	cout << "[CFW] Starting new transaction timeout timer (start=" << dec << timeoutStart << ")" << endl;
}

void CfwTransaction::ackReceived(int seq)
{
	if(messages.empty())
		return;
	mMsg.enter();
	list<CfwMessage*>::iterator iter;
	for(iter = messages.begin(); iter != messages.end(); iter++) {
		if((*iter)->getSeq() == seq) {
			cout << "[CFW] Ack(200) received for tid=" << tid << ", seq=" << dec << seq << endl;
			oldMessages.push_back((*iter));
			break;
		}
	}
	mMsg.leave();
}

void CfwTransaction::run()
{
	cout << "[CFW] Joining transaction thread: " << tid << endl;
	int err = 0;
	struct timeval tv;
	while(!messages.empty() || active) {	// FIXME
		if(!active)
			break;
		if(timeoutTimer != NULL) {	// There's a running timer, check if we need to refresh the transaction
			if((timeoutTimer->getElapsed()-timeoutStart) >= 8000) {	// 8 seconds have passed, the timeout is 10 seconds (80%), send an update...
				cout << "[CFW] Transaction is taking too much time, trigger a 202/'REPORT update' (" << tid << ")" << endl;
				if(status == -1) {	// 202
					stringstream message;
					message << "CFW " << tid << " 202\r\n"
							<< "Timeout: 10\r\n\r\n";	// FIXME (timeout)
					CfwMessage *msg = new CfwMessage(seq, message.str());
					seq++;
					msg->dontWait();
					queueMessage(msg, CFW_202);
				} else {
					stringstream message;
					message << "CFW " << tid << " REPORT\r\n"
							<< "Seq: " << seq << "\r\n"
							<< "Status: update\r\n"
							<< "Timeout: 10\r\n\r\n";	// FIXME (timeout)
					CfwMessage *msg = new CfwMessage(seq, message.str());
					seq++;
					queueMessage(msg, CFW_REPORT_UPDATE);
				}
//				timeoutStart = timeoutTimer->getElapsed();
				startTimer();
			}
		}
		mMsg.enter();
		if(!oldMessages.empty()) {
			while(!oldMessages.empty()) {
				CfwMessage* msg = oldMessages.front();
				oldMessages.pop_front();
				messages.remove(msg);
				cout << "[CFW] Purging old message: seq=" << msg->getSeq() << "(" << tid << ")" << endl;
				if(msg)
					delete msg;
			}
		}
		if(messages.empty()) {
			mMsg.leave();
			if(oldMessages.empty()) {
				if(status == CFW_REPORT_TERMINATE) {
					cout << "[CFW] Status is 'terminate', disactivating transaction...(" << tid << ")" << endl;
					if(timeoutTimer != NULL) {
						// Stop the transaction timeout timer
						delete timeoutTimer;
						timeoutTimer = NULL;
					}
					active = false;
					break;
				} else if(status == CFW_200) {
					cout << "[CFW] Status is '200', disactivating transaction... (" << tid << ")" << endl;
					active = false;
					break;
				}
			}
			if(!running && messages.empty())	// FIXME
				break;
			tv.tv_sec = 0;
			tv.tv_usec = 100000;	// FIXME
			select(0, NULL, NULL, NULL, &tv);
			continue;
		}
		list<CfwMessage*>::iterator iter;
		for(iter = messages.begin(); iter != messages.end(); iter++) {
			CfwMessage *msg = (*iter);
			if(!msg)
				continue;
			if(!msg->getSent()) {	// Send this message now
				string text = msg->getText();
				cout << "[CFW] Sending message: seq=" << dec << msg->getSeq() << ", len=" << dec << (int)text.length() << " (" << tid << ")" << endl;
				if((client == NULL) || (client->getFd() < 0))
					cout << "[CFW] File descriptor is invalid, did the AS go away? (" << tid << ")"<< endl;
				else {	// FIXME
					err = client->sendMessage(text);		// FIXME
					if(err != (int)text.length())
						cout << "[CFW] Error sending message! Sent " << dec << err << " bytes, should have been " << dec << (int)text.length() << " (" << tid << ")" << endl;
						// TODO Should handle this event accordingly...
//						active = false;
//						break;
					else
						cout << "[CFW] Message sent: bytes=" << dec << err << " (" << tid << ")" << endl;			
				}
				if(!msg->isWaiting())	// Not waiting for the 200 ACK
					oldMessages.push_back(msg);
				else
					msg->setSent();
			}
		}
		mMsg.leave();
	}
	cout << "[CFW] Leaving transaction thread: " << tid << endl;
	if(!messages.empty()) {
		mMsg.enter();
		while(!messages.empty()) {
			CfwMessage* msg = messages.front();
			messages.pop_front();
			delete msg;
		}
		mMsg.leave();
	}
	running = false;
	cfw->transactionEnded(tid);
}

int CfwTransaction::report(int newstatus, int timeout, string mime, string blob)
{
	if(newstatus == CFW_202) {	// We handle 202 differently: it's like errors, but has a mandatory timeout attribute
		cout << "[CFW] Ignored 202 from package (202 can be triggered only from the stack)" << endl;
		return 0;	
	}
	if(newstatus != 200) {	/* e.g. 403 */
		errorCode(newstatus);
		return 0;
	}

	stringstream message;
	int messageSeq = 0;
	if(status == -1)		/* Reply with a 200 */
		message << "CFW " << tid << " 200\r\n";
	else	/* Extended transaction, REPORT terminate */
		message << "CFW " << tid << " REPORT\r\n";
	if(seq > 0) {
		messageSeq = seq;
		message << "Seq: " << messageSeq << "\r\n";
	}
	seq++;
	if(status > -1)
		message << "Status: terminate\r\n";
	message << "Timeout: " << timeout << "\r\n";
	if(blob != "")
		message << "Content-Type: " << mime << "\r\nContent-Length: " << blob.length() << "\r\n\r\n" << blob;
	else
		message << "\r\n";

	CfwMessage *msg = new CfwMessage(messageSeq, message.str());
	if(status == -1)
		msg->dontWait();
	queueMessage(msg, (status == -1 ? CFW_200 : CFW_REPORT_TERMINATE));

	return 0;
}

int CfwTransaction::control(string cp, string mime, string blob)
{
	stringstream message;
	message << "CFW " << tid << " CONTROL\r\n";
	seq++;
	message << "Control-Package: " << cp << "\r\n";
//	message << "Content-Type: text/xml\r\n";
	message << "Content-Type: " << mime << "\r\n";
	message << "Content-Length: " << blob.length() << "\r\n\r\n" << blob;

	CfwMessage *msg = new CfwMessage(0, message.str());
	queueMessage(msg, CFW_REPORT_TERMINATE);

//	this->status = CFW_REPORT_TERMINATE;	// FIXME A workaround to end the transaction after the 200 from the AS

	return 0;
}

int CfwTransaction::errorCode(int code, string header, string blob)
{
	int messageSeq = 0;
	stringstream message;
	message << "CFW " << tid << " ";
	switch(code) {
		case 200:
			message << "200\r\n";	// OK
			break;
		case 202:
			message << "202\r\n";	// OK + later REPORTs
			break;
		case 400:
			message << "400\r\n";	// Syntactically incorrect
			break;
		case 403:
			message << "403\r\n";	// Forbidden
			break;
		case 405:
			message << "405\r\n";	// Method not allowed
			break;
		case 420:
			message << "420\r\n";	// Control Package not valid for the current session
			break;
		case 421:
			message << "421\r\n";	// Don't want SYNC renegotiation
			break;
		case 422:
			message << "422\r\n";	// Unsupported Control Package
			break;
		case 423:
			message << "423\r\n";	// Transaction ID already exists
			break;
		case 481:
			message << "481\r\n";	// Target doesn't exist
			break;
		case 500:
			message << "500\r\n";	// Can't understand request
			break;
		default:
			return -1;
	}
	if(header != "")
		message << header;
	if(seq > 0) {
		messageSeq = seq;
		message << "Seq: " << messageSeq << "\r\n";
	}
	seq++;
	if(blob != "")
		message << "Content-Length: " << blob.length() << "\r\n\r\n" << blob;
	else
		message << "\r\n";

	CfwMessage *msg = new CfwMessage(messageSeq, message.str());
	msg->dontWait();
//	queueMessage(msg);

	if((code != 202) && (code != 423))
		queueMessage(msg, CFW_REPORT_TERMINATE);
//		status = CFW_REPORT_TERMINATE;
	else
		queueMessage(msg);

	return 0;
}


// The stack
CfwStack::CfwStack(InetHostAddress &address, unsigned short int port, bool hardKeepAlive)
{
	cout << "[CFW] Creating new CFW stack: " << address.getHostname() << ":" << dec << port << endl;
	alive = true;
	this->port = port;
	this->address = address;
	this->hardKeepAlive = hardKeepAlive;
	supportedPackages = "";
	endedTransactions.clear();
	endedDialogs.clear();
	pkgSharedObjects.clear();
	cfwManager = NULL;
}

CfwStack::~CfwStack()
{
	cout << "[CFW] Destroying CFW stack: " << address.getHostname() << ":" << dec << port << endl;
	// First destroy all packages
	if(!pkgFactories.empty()) {
		while(!pkgFactories.empty()) {
			ControlPackageFactory *pkgF = pkgFactories.front();
			pkgFactories.pop_front();
			cout << "[CFW]\t\tDestroying package: " << pkgF->getName() << endl;
			delete pkgF;
		}
	}
	if(!pkgSharedObjects.empty()) {
		void *plugin = NULL;
		while(!pkgSharedObjects.empty()) {
			plugin = pkgSharedObjects.front();
			pkgSharedObjects.pop_front();
			if(plugin)
				dlclose(plugin);
		}
	}
	shutdownServer(server);
	if(alive) {
		alive = false;
		join();
	}
}

void CfwStack::setCfwManager(CfwManager *cfwManager)
{
	this->cfwManager = cfwManager;
	// Load all the plugins
	loadPackages();
	cout << "[CFW] List of loaded packages:" << endl;
	list<ControlPackage *>::iterator iter;
	stringstream supported;
	for(iter = packages.begin(); iter != packages.end(); iter++ ) {
		cout << "[CFW]     " << (*iter)->getName() << "/" << (*iter)->getVersion() << " (" << (*iter)->getDescription() << "), " << (*iter)->getMimeType() << endl;
		if(supportedPackages == "")	// First add
			supported << (*iter)->getName() << "/" << (*iter)->getVersion();
		else
			supported << "," << (*iter)->getName() << "/" << (*iter)->getVersion();
		supportedPackages = supported.str();
	}
	cout << "[CFW] Supported: " << supportedPackages << endl;
}

string CfwStack::getInfo(string about)
{
	if(about == "")
		return "";
	int what = 0;
	if(about == "all")
		what = 1;
	else if(about == "transactions")
		what = 2;
	else if(about == "clients")
		what = 3;
	else {		// See if the requested package exists
		list<ControlPackage *>::iterator iter;
		for(iter = packages.begin(); iter != packages.end(); iter++) {
			ControlPackage *pkg = (*iter);
			if(!pkg)
				continue;
			if(pkg->getName() == about) {
				what = 4;
				break;
			}
		}
	}
	if(what == 0)
		return "";
	stringstream info;
	info << "CFW:" << "\r\n";
	if(what == 1)
		info << "\t" << address << ":" << dec << port << "\r\n";
	if(what < 3) {
		info << "\tTransactions:" << "\r\n";
		if(transactions.empty())
			info << "\t\tnone" << "\r\n";
		else {
			mTransactions.enter();
			map<string, CfwTransaction *>::iterator t;
			CfwTransaction *tr = NULL;
			for(t = transactions.begin(); t != transactions.end(); t++) {
				tr = t->second;
				if(tr == NULL)
					continue;
				info << "\t\t" << tr->getTransaction() << "\r\n";
				string statusString = "";
				switch(tr->getStatus()) {
					case CFW_200:
						statusString = "200";
						break;
					case CFW_REPORT_UPDATE:
						statusString = "update";
						break;
					case CFW_REPORT_TERMINATE:
						statusString = "terminate";
						break;
					default:
						statusString = "unknown";
						break;
				}
				info << "\t\t\tStatus: " << dec << statusString << "\r\n";
				info << "\t\t\tfd: " << dec << (tr->getClient() ? tr->getClient()->getFd() : -1) << "\r\n";
			}
			mTransactions.leave();
			mEnded.enter();
			if(!endedTransactions.empty()) {
				info << "\tEnded Transactions:" << "\r\n" << "\t\t";
				list<string>::iterator ended;
				for(ended = endedTransactions.begin(); ended != endedTransactions.end(); ended++)
					info << (*ended) << " ";
				info << "\r\n";
			}
			mEnded.leave();
		}
	}
	if(what == 2)
		return info.str();
	if((what == 1) || (what == 3)) {
		info << "\tClients:" << "\r\n";
		if(clients.empty())
			info << "\t\tnone" << "\r\n";
		else {
			list<MediaCtrlClient *>::iterator iter;
			mClients.enter();
			for(iter = clients.begin(); iter != clients.end(); iter++ ) {
				MediaCtrlClient *client = (*iter);
				if(client == NULL)
					continue;
				info << "\t\t" << client->getDialogId() << "\r\n";
				info << "\t\t\t" << "From: " << client->getIp() << ":" << dec << client->getPort() << "\r\n";
				info << "\t\t\t" << "Authenticated: " << client->getAuthenticated() << "\r\n";
				info << "\t\t\t" << "fd: " << dec << client->getFd() << "\r\n";
			}
			mClients.leave();
		}
	}
	if(what == 3)
		return info.str();
	info << "\tPackages:" << "\r\n";
	list<ControlPackage *>::iterator iter;
	for(iter = packages.begin(); iter != packages.end(); iter++) {
		ControlPackage *pkg = (*iter);
		if(!pkg)
			continue;
		if(what == 4) {
			if(pkg->getName() != about)
				continue;
		}
		info << "\t\t" << pkg->getName() << "/" << pkg->getVersion() << "\r\n";
		string pkgInfo = pkg->getInfo();
		if(pkgInfo == "")
			return "";
		string::size_type index;
		string line = "";
		int skip = 0;
		while(1) {
			index = pkgInfo.find("\r\n");
			if(index != string::npos)
				skip = 2;
			else {
				index = pkgInfo.find("\n");
				if(index != string::npos)
					skip = 1;
				else
					break;
			}
			line = pkgInfo.substr(0, index);
			info << "\t\t\t" << line << "\r\n";
			pkgInfo = pkgInfo.substr(index+skip);
		}
	}
	return info.str();
}

void CfwStack::loadPackages()
{
	DIR *dir = NULL;
	struct dirent *plugin = NULL;
	// Take all shared objects and try to load them
	string path = cfwManager->getConfValue("packages", "path");
	if(path == "")
		path = "./packages";
	cout << "[CFW] Packages folder: " << path << endl;
	dir = opendir(path.c_str());	// FIXME
	if(!dir) {
		cout << "[CFW] Couldn't access 'packages' folder (" << path << ")! No plugins will be used..." << endl;
		return;
	}
	char pluginpath[255];
	while((plugin = readdir(dir))) {
		int len = strlen(plugin->d_name);
		if (len < 4)
			continue;
		if (strcasecmp(plugin->d_name+len-3, ".so"))
			continue;
		cout << "[CFW] Loading plugin '" << plugin->d_name << "'..." << endl;
		memset(pluginpath, 0, 255);
		sprintf(pluginpath, "%s/%s", path.c_str(), plugin->d_name);
		void *package = dlopen(pluginpath, RTLD_LAZY);
		if (!package)
			cout << "[CFW]     Couldn't load plugin '" << plugin->d_name << "': " << dlerror() << endl;
		else {
			create_cp *create_p = (create_cp*) dlsym(package, "create");
			const char *dlsym_error = dlerror();
			if (dlsym_error) {
				cout << "[CFW]     Couldn't load symbol 'create': " << dlsym_error << endl;
				continue;
			}
			destroy_cp *destroy_p = (destroy_cp*) dlsym(package, "destroy");
			dlsym_error = dlerror();
			if (dlsym_error) {
				cout << "[CFW]     Couldn't load symbol 'destroy': " << dlsym_error << endl;
				return;
			}
			ControlPackage* newpkg = create_p(this);
			if(!newpkg) {
				cout << "[CFW]     Couldn't create the new package instance..." << endl;
				continue;
			}
			if(!newpkg->setup()) {
				cout << "[CFW]     Couldn't initialize and setup the new " << newpkg->getName() << " instance..." << endl;
				continue;
			}
			if(getCollector())
				cout << "[CFW]     Setting valid collector" << endl;
			else
				cout << "[CFW]     Setting INVALID collector" << endl;
			newpkg->setCollector(getCollector());
			packages.push_back(newpkg);
			pkgSharedObjects.push_back(package);
			ControlPackageFactory *newPkgFactory = new ControlPackageFactory(newpkg, destroy_p);
			pkgFactories.push_back(newPkgFactory);
		}
	}
	closedir(dir);
}

int CfwStack::addClient(string callId, string cfwId, string ip, uint16_t port, bool tls, string fingerprint)
{
	// First check if such a client already exists
	list<MediaCtrlClient *>::iterator iter;
	mClients.enter();
	for(iter = clients.begin(); iter != clients.end(); iter++ ) {
		if(cfwId == (*iter)->getDialogId()) {
			mClients.leave();
			cout << "[CFW] This client already exists!" << endl;
			return 488;	// FIXME
		}
	}
	MediaCtrlClient *client = new MediaCtrlClient(this, tls, fingerprint);
	cout << "[CFW] Adding new client from SIP Call-ID " << callId << "... (" << cfwId << ")" << endl;
	client->setDialog(callId, cfwId);
	client->setAddress(ip, port);
	client->start();
	// Add client to list
	clients.push_back(client);
	mClients.leave();
	return 200;	// FIXME return SIP code? (e.g. 200)
}

int CfwStack::removeClient(string cfwId)
{
	// Destroy CFW connection and remove client from the list (FIXME destroy its conferences, dialogs etc too? currently it doesn't)
	list<MediaCtrlClient *>::iterator iter;
	bool found = false;
	mClients.enter();
	for(iter = clients.begin(); iter != clients.end(); iter++ ) {
		if(cfwId == (*iter)->getDialogId()) {
			found = true;
			MediaCtrlClient *client = (*iter);
			cout << "[CFW] Removing client associated with Dialog-ID " << cfwId << endl;
			clientsMap.erase(client->getFd());
			clients.erase(iter);
			struct timeval tv = {0, 200000};	// FIXME
			select(0, NULL, NULL, NULL, &tv);
			delete client;
			break;
		}
	}
	mClients.leave();
	if(found)
		return 0;
	cout << "[CFW] Couldn't find any client associated with Dialog-ID " << cfwId << endl;
	return -1;
}

MediaCtrlClient *CfwStack::getClient(int fd)
{
	MediaCtrlClient *client = NULL;
	list<MediaCtrlClient *>::iterator iter;
	mClients.enter();
	for(iter = clients.begin(); iter != clients.end(); iter++ ) {
		if(fd == (*iter)->getFd()) {
			client = (*iter);
			return client;
		}
	}
	mClients.leave();
	// Fallthrough
	cout << "[CFW] Couldn't find any client associated with fd=" << dec << fd << endl;
	return NULL;
}

ControlPackage *CfwStack::getPackage(string name)
{
	list<ControlPackage *>::iterator iter;
	for(iter = packages.begin(); iter != packages.end(); iter++ ) {
		if(name == (*iter)->getName())
			return (*iter);
	}
	return NULL;
}

string CfwStack::getSupportedPackages()
{
	return supportedPackages;
}

list<string> CfwStack::getPackages()
{
	if(packagesString.empty()) {
		string pkg;
		list<ControlPackage *>::iterator iter;
		for(iter = packages.begin(); iter != packages.end(); iter++) {
			pkg = (*iter)->getName() + "/" + (*iter)->getVersion();
			packagesString.push_back(pkg);
		}
	}
	return packagesString;
}

string CfwStack::matchPackages(string pkgs, bool *ok)
{
	stringstream required, supported;
	string unrequired = supportedPackages;
	required << "Packages: ";
	supported << "Supported: ";
	regex crlf(","), re;
	cmatch matches;
	sregex_token_iterator a(pkgs.begin(), pkgs.end(), crlf, -1);
	sregex_token_iterator b;
	string pkg = "";
	if((a == b) || (*a == "")) {	// No matches
		*ok = false;
		supported << supportedPackages << "\r\n";
		return supported.str();
	}
	bool first = true;
	while(1) {
		pkg = *a;
		// First check if this package is in a valid format
		re.assign("(\\S+)\\/(\\d)\\.(\\d)", regex_constants::icase);
		if(!regex_match(pkg.c_str(), matches, re)) {
			// Invalid package
			cout << "[CFW] Invalid package: " << pkg << endl;
			*ok = false;
			supported << supportedPackages << "\r\n";
			return supported.str();
		}
		cout << "[CFW] Checking " << matches[0] << "... ";
		string::size_type loc = unrequired.find(matches[0], 0);
		if(loc == string::npos) {	// Not found
			cout << "Failed" << endl;
			cout << "[CFW]     !!! This will result in a 422 error message !!!" << endl;
			*ok = false;
			supported << supportedPackages << "\r\n";
			return supported.str();
		}
		// Add the package to the required ones...
		cout << "OK" << endl;
		if(first) {
			required << matches[0];
			first = false;
		} else
			required << "," << matches[0];
		// ...and remove it from the 'unrequired' list
		unrequired.erase(loc, matches[0].length());
		*a++;
		if((a == b) || (*a == ""))
			break;
	}
	// Now cleanup the unrequired string from extra commas
	string::size_type comma;
	int counter = 0;
	if(unrequired[0] == ',')
		unrequired.erase(0, 1);
	while(1) {
		counter = 0;
		comma = unrequired.find(" ,", 0);
		if(comma == string::npos)
			counter++;
		else
			unrequired.erase(comma+1, 1);
		comma = unrequired.find(",,", 0);
		if(comma == string::npos)
			counter++;
		else
			unrequired.erase(comma, 1);
		comma = unrequired.find(",", unrequired.length()-1);
		if(comma == string::npos)
			counter++;
		else
			unrequired.erase(comma, 1);
		if(counter == 3)
			break;
	}
	required << "\r\n" << supported.str() << unrequired << "\r\n";
	return required.str();
}

int CfwStack::matchConnection(int fd, struct sockaddr_in *client)
{
	char *client_ip = inet_ntoa(client->sin_addr);
	unsigned short int client_port = ntohs(client->sin_port);

	cout << "[CFW] Matching fd=" << dec << fd << " with transport address " << client_ip << ":" << dec << client_port << endl;

	list<MediaCtrlClient *>::iterator iter;
	bool found = false;
	mClients.enter();
	for(iter = clients.begin(); iter != clients.end(); iter++ ) {
		if((client_port == (*iter)->getPort()) && !strcasecmp(client_ip, (*iter)->getIp().c_str())) {
			MediaCtrlClient *client = (*iter);
			found = true;
			client->setFd(fd);
			clientsMap[fd] = client;	// Update the map
			break;
		}
	}
	mClients.leave();
	if(found)
		return 0;
	return -1;
}

void CfwStack::transactionEnded(string tid)
{
	mEnded.enter();
	endedTransactions.push_back(tid);
	mEnded.leave();
}

CfwTransaction *CfwStack::getTransaction(string tid)
{
	return transactions[tid];
}

void CfwStack::removeTransaction(string tid)
{
	mTransactions.enter();
	CfwTransaction *t = transactions[tid];
	if(t == NULL) {
		mTransactions.leave();
		return;
	}
	transactions.erase(tid);
	mTransactions.leave();
	delete t;
}

void CfwStack::endDialog(MediaCtrlClient *client)
{
	if(client == NULL)
		return;
	endedDialogs.push_back(client->getCallId());
	// All the currently active transactions related to this client keep living, but reports are invalidated
	if(transactions.empty())
		return;
	mTransactions.enter();
	map<string, CfwTransaction *>::iterator iter;
	CfwTransaction *t = NULL;
	for(iter = transactions.begin(); iter != transactions.end(); iter++) {
		t = iter->second;
		if((t != NULL) && (t->getClient() == client))
			t->setClient(NULL);	// FIXME wrong
	}
	mTransactions.leave();
}

void CfwStack::report(ControlPackage *cp, void *requester, string tid, int status, int timeout, string blob)
{
	cout << "[CFW] \tReceived '";
	if(status == CFW_202)
		cout << "202";
	else if(status == CFW_403)
		cout << "403";
	else cout << "REPORT";
	cout << "' from " << cp->getName() << endl;
	cout << "[CFW] \t\t" << tid << ", " << status << ", " << timeout;
	if(blob != "")
		cout << ", " << blob << ", " << blob.length();
	cout << endl;
	CfwTransaction *transaction = transactions[tid];
	if(!transaction) {
		cout << "[CFW] \t\tThis transaction doesn't exist" << endl;
		// TODO ??? received REPORT for a transaction that doesn't exist (anymore?)
		return;
	}

	// TODO Should check if the MediaCtrlClient in the transaction and the 'requester' match
	if(transaction->getClient() != (MediaCtrlClient*)requester) {
		cout << "[CFW] \t\tInvalid MediaCtrlClient!" << endl;
		cout << "[CFW] \t\t\tTransaction: " << (transaction->getClient() ? transaction->getClient()->getDialogId() : "(null)") << endl;
		cout << "[CFW] \t\t\tPackage:     " << (requester ? ((MediaCtrlClient*)requester)->getDialogId() : "(null)") << endl;
		return;	// FIXME
	}

	transaction->report(status, timeout, cp->getMimeType(), blob);
}

void CfwStack::control(ControlPackage *cp, void *requester, string blob)
{
	cout << "[CFW] \tReceived 'CONTROL' from " << cp->getName() << endl;
	cout << "[CFW] \t\t" << blob << ", " << blob.length() << endl;
	// TODO
	string tid = "";
	while(1) {
		tid = random_tid();
		if(transactions[tid] == NULL)	// We found an unused transaction identifier
			break;
	}
	if(requester == NULL)
		cout << "[CFW] \t\t\tThe 'requester' pointer is invalid, expect problems..." << endl;
	CfwTransaction *transaction = new CfwTransaction(this, (MediaCtrlClient *)requester, tid);	// FIXME badly!
	transactions[tid] = transaction;
	transaction->control(cp->getName() + "/" + cp->getVersion(), cp->getMimeType(), blob);
}

ControlPackageConnection *CfwStack::getConnection(ControlPackage *cp, string conId)
{
	if(conId == "")
		return NULL;
	cout << "[CFW] \t" << cp->getName() << " requested connection " << conId << endl;
	MediaCtrlEndpoint *endpoint = cfwManager->getEndpoint(cp, conId);
	return (endpoint ? endpoint->getCpConnection() : NULL);
}

ControlPackageConnection *CfwStack::createConference(ControlPackage *cp, string confId)
{
	cout << "[CFW] \t" << cp->getName() << " requested creation of a new conference: " << confId << endl;
	MediaCtrlEndpoint *endpoint = cfwManager->createConference(cp, confId);
	return (endpoint? endpoint->getCpConnection() : NULL);
}

void CfwStack::dropConnection(ControlPackage *cp, ControlPackageConnection *connection)
{
	if(connection == NULL)
		return;
	cout << "[CFW] \t" << cp->getName() << " dropped connection " << connection->getConnectionId() << endl;
	MediaCtrlEndpoint *endpoint = (MediaCtrlEndpoint *)connection->getEndpoint();
	if(endpoint == NULL)
		return;
	endpoint->decreaseCounter();
}

string CfwStack::getPackageConfValue(ControlPackage *cp, string element, string attribute)
{
	cout << "[CFW] \t" << cp->getName() << " requested a configuration value: (" << element;
	if(attribute != "")
		cout << ", " << attribute;
	cout << ")" << endl;
	return cfwManager->getPackageConfValue(cp->getName(), element, attribute);
}

void CfwStack::sendFrame(ControlPackageConnection *connection, MediaCtrlFrame *frame)
{
	if(connection == NULL)
		return;
	MediaCtrlEndpoint *endpoint = (MediaCtrlEndpoint *)connection->getEndpoint();
	if(endpoint == NULL)
		return;
	endpoint->sendFrame(frame);
}

void CfwStack::incomingFrame(ControlPackageConnection *connection, MediaCtrlFrame *frame)
{
	if(connection == NULL)
		return;
	MediaCtrlEndpoint *endpoint = (MediaCtrlEndpoint *)connection->getEndpoint();
	if(endpoint == NULL)
		return;
	if(endpoint->getType() != MEDIACTRL_CONFERENCE)
		return;	// This method is only needed to make other packages aware of "incoming" frames on a conference connection
	MediaCtrlConference *conference = (MediaCtrlConference *)endpoint;
	if(conference != NULL) {
		conference->incomingFrame(frame);
	}
}

void CfwStack::clearDtmfBuffer(ControlPackageConnection *connection)
{
	if(connection == NULL)
		return;
	MediaCtrlEndpoint *endpoint = (MediaCtrlEndpoint *)connection->getEndpoint();
	if(endpoint == NULL)
		return;
	endpoint->clearDtmfBuffer();
}

int CfwStack::getNextDtmfBuffer(ControlPackageConnection *connection)
{
	if(connection == NULL)
		return -1;
	MediaCtrlEndpoint *endpoint = (MediaCtrlEndpoint *)connection->getEndpoint();
	if(endpoint == NULL)
		return -1;
	return endpoint->getNextDtmfBuffer();
}

uint32_t CfwStack::getFlags(ControlPackageConnection *connection, int mediaType)
{
	if(connection == NULL)
		return 0;
	MediaCtrlEndpoint *endpoint = (MediaCtrlEndpoint *)connection->getEndpoint();
	if(endpoint == NULL)
		return 0;
	return endpoint->getFlags(mediaType);
}

ControlPackageConnection *CfwStack::getSubConnection(ControlPackageConnection *connection, int mediaType)
{
	if(connection == NULL)
		return NULL;
	if(mediaType != MEDIACTRL_MEDIA_AUDIO)
		return NULL;
	MediaCtrlEndpoint *endpoint = (MediaCtrlEndpoint *)connection->getEndpoint();
	if(endpoint == NULL)
		return NULL;
	if(endpoint->getType() != MEDIACTRL_CONNECTION)
		return NULL;	// FIXME Should we return the conference itself, instead?
	MediaCtrlConnection* endpointC = (MediaCtrlConnection*)endpoint;
	if(endpointC == NULL)
		return NULL;
	MediaCtrlConnection* endpointOwned = endpointC->getOwned(mediaType);
	if(endpointOwned == NULL)
		return NULL;
	return endpointOwned->getCpConnection();
}

ControlPackageConnection *CfwStack::getSubConnection(ControlPackageConnection *connection, string label)
{
	if(connection == NULL)
		return NULL;
	if(label == "")
		return NULL;
	MediaCtrlEndpoint *endpoint = (MediaCtrlEndpoint *)connection->getEndpoint();
	if(endpoint == NULL)
		return NULL;
	if(endpoint->getType() != MEDIACTRL_CONNECTION)
		return NULL;	// FIXME Should we return the conference itself, instead?
	MediaCtrlConnection* endpointC = (MediaCtrlConnection*)endpoint;
	if(endpointC == NULL)
		return NULL;
	MediaCtrlConnection* endpointOwned = endpointC->getOwned(label);
	if(endpointOwned == NULL)
		return NULL;
	return endpointOwned->getCpConnection();
}

MediaCtrlFrame *CfwStack::decode(MediaCtrlFrame *frame)
{
	if(cfwManager)
		return cfwManager->decode(frame);
	return NULL;
}

MediaCtrlFrame *CfwStack::encode(MediaCtrlFrame *frame, int dstFormat)
{
	if(cfwManager)
		return cfwManager->encode(frame, dstFormat);
	return NULL;
}


void CfwStack::thread()
{
	// Setup the CFW server
	struct sockaddr_in server_address, client_address;
	server_address.sin_family = AF_INET;
	server_address.sin_port = htons(port);
	server_address.sin_addr.s_addr = INADDR_ANY;
	server = socket(AF_INET, SOCK_STREAM, 0);
	int yes = 1;
	assert(setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) > -1);
	assert(bind(server, (struct sockaddr *)(&server_address), sizeof(struct sockaddr)) > -1);
	listen(server, 5);
	cout << "[CFW] CFW server listening on " << port << endl;
	fds.clear();
	int g=0;
	for(g = 1; g < 10; g++) {
		pollfds[g].fd = -1;
		pollfds[g].events = 0;
		pollfds[g].revents = 0;
	}
	pollfds[0].fd = server;
	pollfds[0].events = POLLIN;
	pollfds[0].revents = 0;
	fds.push_back(server);
	int err = 0, fd = 0, client = 0;
	uint16_t i = 0;
	socklen_t addrlen = sizeof(struct sockaddr);

	while(alive) {
		// Free dead transactions before polling
		mEnded.enter();
		if(!endedTransactions.empty()) {
			while(!endedTransactions.empty()) {
				string tid = endedTransactions.front();
				endedTransactions.pop_front();
				removeTransaction(tid);
			}
		}
		mEnded.leave();
		// TODO Check if any of the K-Alive timers has expired (currently we don't check it for debugging purposes)
//		if(0) {
			list<MediaCtrlClient*>::iterator iter;
			for(iter = clients.begin(); iter != clients.end(); iter++) {
				MediaCtrlClient *client = (*iter);
				if(!client)
					continue;
				if(client->getTimeout()) {
					cout << "[CFW] K-Alive timeout reached for " << client->getDialogId() << endl;
					// Send BYE, instead of just resetting the counter?
					client->setKeepAlive(client->getKeepAlive());
					if(hardKeepAlive) {	// Send BYE
						cout << "[CFW] \tSending BYE" << endl;
						endDialog(client);
					} else
						cout << "[CFW] \tJust resetting the counter... (debug method)" << endl;
				}
			}
//		}
		// Free ended dialogs before proceeding
		if(!endedDialogs.empty()) {
			string callId = "";
			while(!endedDialogs.empty()) {
				callId = endedDialogs.front();
				endedDialogs.pop_front();
				if(callId != "")
					cfwManager->endDialog(callId);
			}
		}
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
						client = accept(fd, (struct sockaddr *)(&client_address), &addrlen);
						if(client < 0)
							continue;
						// TODO Match this connection to the right MediaCtrlClient
						if(matchConnection(client, &client_address) < 0) {
							cout << "[CFW] Shutting down unexpected connection from " << inet_ntoa(client_address.sin_addr) << ":" << ntohs(client_address.sin_port) << endl;
							close(client);
							continue;
						}
						cout << "[CFW] Accepted incoming connection from " << inet_ntoa(client_address.sin_addr) << ":" << ntohs(client_address.sin_port) << endl;
#if 0
						// Add client to list of known fds
						fds.push_back(client);
						pollfds[fds.size()-1].fd = client;
						pollfds[fds.size()-1].events = POLLIN;
						pollfds[fds.size()-1].revents = 0;
					} else {	// There's data to read from a client
						receiveMessage(fd);
#endif
					}
				}
			}
		}
	}

	cout << "[CFW] Leaving thread..." << endl;
//	delete this;
}

void CfwStack::connectionLost(int fd) {
	cout << "[CFW] Lost connection to fd=" << fd << endl;
	mClients.enter();
	MediaCtrlClient *client = clientsMap[fd];
	if(client) {
		clientsMap.erase(client->getFd());
		endDialog(client);
	}
	mClients.leave();
}

void CfwStack::parseMessage(MediaCtrlClient *client, string message)
{
	if(client == NULL) {
		// TODO Should we reply somehow?
		cout << "[CFW] Invalid MediaCtrlClient" << endl;
		return;
	}
	regex crlf("\r\n"), re;
	cmatch matches;
	sregex_token_iterator a(message.begin(), message.end(), crlf, -1);
	sregex_token_iterator b;
	string line = *a;
	cout << "[CFW] Parsing header: " << endl;
	re.assign("(CFW (\\w+) (\\D+))|(CFW (\\w+) (\\d+)(\\s((\\S+\\s?)+))?)", regex_constants::icase);
	if(!regex_match(line.c_str(), matches, re)) {
		// TODO Should we reply somehow?
		cout << "[CFW] Invalid header: " << line << endl;
		return;
	}
	string request, tid;
	if(matches[1].first != matches[1].second) {	// First match, it's a directive and not an error code
		request = matches[3];
		for(uint16_t i=0; i < request.length(); ++i)	// We check the request in upper case (this makes it case insensitive)
			request[i] = toupper(request[i]);
		tid = matches[2];
	} else {	// Second match, it's an error code (and the optional comment, if available)
		request = matches[6];
		tid = matches[5];
		// We skip the comment, which might be in matches[8]
	}
	if((request != "SYNC") && (!client->getAuthenticated())) {
		CfwTransaction *transaction = new CfwTransaction(this, client, tid);
		transactions[tid] = transaction;
		transaction->errorCode(403);	// FIXME
		return;
	}
	if(request == "200") {		// 200
		CfwTransaction *transaction = transactions[tid];
		if(!transaction) {
			cout << "[CFW] \t\tThis transaction doesn't exist" << endl;
			// TODO ??? to a non-existent transaction? (we send 481 for now)
			CfwTransaction *transaction = new CfwTransaction(this, client, tid);
			transactions[tid] = transaction;
			transaction->errorCode(481);	// FIXME
			return;
		}
		int seqno = 0;
		while(1) {
			*a++;	// Go to the next line
			if(a == b)
				break;
			line = *a;
			if(line == "")
				break;
			re.assign("Seq: (\\d+)", regex_constants::icase);
			if(!regex_match(line.c_str(), matches, re)) {
				cout << "[CFW] Invalid line: _ " << line << endl;
				transaction->errorCode(400);	// FIXME
				return;
			}
			string seq = matches[1];
			seqno = atoi(seq.c_str());
		}
		// Handle correct 200 to previously sent message
		cout << "[CFW] \t\tSequence: " << seqno << endl;
		if(seqno >= 0)
			transaction->ackReceived(seqno);
	} else if(request == "K-ALIVE") {		// K-ALIVE
		while(1) {
			*a++;	// Go to the next line
			if(a == b)
				break;
			line = *a;	// K-ALIVE is supposed not to have any lines
			if(line == "")
				break;
			cout << "[CFW] Invalid line: _ " << line << endl;
			CfwTransaction *transaction = new CfwTransaction(this, client, tid);
			transaction->errorCode(400);	// FIXME
			transactions[tid] = transaction;
			return;
		}
		CfwTransaction *transaction = new CfwTransaction(this, client, tid);
		client->setKeepAlive(client->getKeepAlive());	// K-ALIVE refreshes the timeout
		transactions[tid] = transaction;
		transaction->errorCode(200);
		return;
	} else if(request == "SYNC") {		// SYNC
		CfwTransaction *transaction = transactions[tid];
		if(transaction) {
			cout << "[CFW] \t\tThis transaction already exists" << endl;
			// TODO ??? to already existent transaction?
			transaction->errorCode(423);
			return;
		}
		transaction = new CfwTransaction(this, client, tid);
		transactions[tid] = transaction;
		// Parse the header lines
		string dialogid = "", kalive = "", cpackages = "";
		while(1) {
			*a++;
			if(a == b)
				break;
			line = *a;
			if(line == "")
				break;
			re.assign("Dialog-ID: (\\S+)", regex_constants::icase);
			if(regex_match(line.c_str(), matches, re)) {
				dialogid = matches[1];
				continue;
			}
			re.assign("Keep-Alive: (\\d+)", regex_constants::icase);
			if(regex_match(line.c_str(), matches, re)) {
				kalive = matches[1];
				continue;
			}
			re.assign("Packages: (\\S+)", regex_constants::icase);
			if(regex_match(line.c_str(), matches, re)) {
				cpackages = matches[1];
				continue;
			}
			cout << "[CFW] Invalid line: _ " << line << endl;
			transaction->errorCode(400);	// FIXME
			endDialog(client);	// FIXME
			return;
		}
		// Check the Dialog-ID
		if(dialogid == "") {
			cout << "[CFW] SYNC request is missing the Dialog-ID" << endl;
			// 481 error code (target does not exist)
			transaction->errorCode(481);
			endDialog(client);	// FIXME
			return;
		}
		if(client->getDialogId() != dialogid) {
			cout << "[CFW] Dialog-ID " << dialogid << " in SYNC request doesn't match" << endl;
			transaction->errorCode(481);
			// TODO 481 error code (target does not exist)
			endDialog(client);	// FIXME
			return;
		} else {
			client->setAuthenticated();
			if(client->getAuthenticated())
				cout << "[CFW] \tClient correctly correlated and authenticated" << endl;
			cout << "[CFW] Dialog-ID " << dialogid << " in SYNC request matches" << endl;
		}
		// Now check the Keep-Alive
		if(kalive == "") {
			cout << "[CFW] SYNC request is missing the Keep-Alive" << endl;
			// TODO 481 error code (target does not exist) FIXME
			transaction->errorCode(481);
			endDialog(client);	// FIXME
			return;
		}
		uint16_t k = atoi(kalive.c_str());
		stringstream header;
		header << "Keep-Alive: " << dec << k << "\r\n";
		client->setKeepAlive(k);
		// Finally check the requested Control Packages
		if(cpackages == "") {
			cout << "[CFW] SYNC request is missing the Packages" << endl;
			// TODO 481 error code (target does not exist) FIXME
			transaction->errorCode(481);
			endDialog(client);	// FIXME
			return;
		}
		cout << "[CFW] Requested Packages are " << cpackages << ", trying to match them..." << endl;
		bool ok = true;
		string res = matchPackages(cpackages, &ok);
		if(ok) {
			header << res;
			transaction->errorCode(200, header.str());
		} else {
			transaction->errorCode(422, res);
			endDialog(client);	// FIXME
			return;
		}
	} else if(request == "CONTROL") {	// CONTROL
		CfwTransaction *transaction = transactions[tid];
		if(transaction) {
			cout << "[CFW] \t\tThis transaction already exists" << endl;
			transaction->errorCode(423);	// FIXME
			return;
		}
		transaction = new CfwTransaction(this, client, tid);
		transactions[tid] = transaction;
		// Parse the header lines
		string cp = "", content = "", mimeType = "";
		while(1) {
			*a++;
			if(a == b)
				break;
			line = *a;
			if(line == "")
				break;
			re.assign("Content-Length: (\\d+)", regex_constants::icase);
			if(regex_match(line.c_str(), matches, re)) {
				content = matches[1];
				continue;
			}
			re.assign("Content-Type: (\\S+)", regex_constants::icase);
			if(regex_match(line.c_str(), matches, re)) {
				cout << "[CFW] Content-Type is " << matches[1] << endl;
				mimeType = matches[1];
				// TODO Should match Content-Type and Control-Package
				continue;
			}
			re.assign("Control-Package: ((\\D+)(\\/(\\d)\\.(\\d))?)", regex_constants::icase);
			if(regex_match(line.c_str(), matches, re)) {
				cp = matches[2];	// We only need the name, not the version
				continue;
			}
			cout << "[CFW] Invalid line: _ " << line << endl;
			transaction->errorCode(400);	// FIXME
			endDialog(client);	// FIXME
			return;
		}
		// Check the Control-Package header
		if(cp == "") {
			cout << "[CFW] CONTROL request is missing the Control-Package" << endl;
			transaction->errorCode(400);	// FIXME
			return;
		}
		ControlPackage *package = getPackage(cp);
		bool fail = false;
		if(!package) {
			cout << "[CFW] CONTROL request is for invalid Control-Package " << cp << endl;
			fail = true;
		}
		if(!fail && (package->getMimeType() != mimeType)) {
			cout << "[CFW] CONTROL request has a Content Type which doesn't match the addressed Control-Package " << cp << endl;
			fail = true;
		}
		// Check the Content-Length header
		string controlBlob = "";
		int len = 0;
		if(content != "") {
			len = atoi(content.c_str());
			if(len > 0)
				controlBlob = client->getContent(this, len);
		}
		if(fail) {
			transaction->errorCode(420);	// Unsupported Control Package
			return;
		}
		cout << "[CFW] XML Blob:" << endl;
		cout << (controlBlob != "" ? controlBlob : "(no content)") << endl;
		transaction->startTimer();	// The package may take longer than expected
		package->control(client, tid, controlBlob);
		return;
	} else {	// FIXME Handle error codes the AS might be sending us
		CfwTransaction *transaction = transactions[tid];
		if(!transaction) {
			cout << "[CFW] \t\tThis transaction doesn't exist (but the method is unsupported anyway)" << endl;
			transaction = new CfwTransaction(this, client, tid);
			transactions[tid] = transaction;
		}
		transaction->errorCode(405);	// Unsupported primitive/method
		return;
	}
}
