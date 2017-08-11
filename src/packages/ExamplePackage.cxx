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
 * \brief Dummy Example Control Package (sample)
 *
 * \author Lorenzo Miniero <lorenzo.miniero@unina.it>
 *
 * \ingroup packages
 * \ref packages
 */

#include "expat.h"
#include "ControlPackage.h"

using namespace mediactrl;


// eXpat parser callbacks for parsing msc-example-package XML blobs
static void XMLCALL startElement(void *msg, const char *name, const char **atts);
static void XMLCALL endElement(void *msg, const char *name);

class ExamplePackage;

/// ExamplePackage incoming CONTROL message
class ExamplePackageMessage : public gc {
	public:
		ExamplePackage *pkg;
		string tid;
		string blob;
		uint32_t len;
		void *sender;

		int level;
};

/// Example (dummy) package class
class ExamplePackage : public ControlPackage {
	public:
		ExamplePackage();
		~ExamplePackage();

		bool setup();
		string getInfo();

		void control(void *sender, string tid, string blob);

		void setCollector(void *frameCollector);

		void handleDummy(ExamplePackageMessage *msg, const char *op, const char *var);

		void sendFrame(ControlPackageConnection *connection, MediaCtrlFrame *frame) { return; };
		void incomingFrame(ControlPackageConnection *connection, ControlPackageConnection *subConnection, MediaCtrlFrame *frame) { return; };
		void incomingDtmf(ControlPackageConnection *connection, ControlPackageConnection *subConnection, int type) { return; };
		void frameSent(ControlPackageConnection *connection, ControlPackageConnection *subConnection, MediaCtrlFrame *frame) { return; };
		void connectionLocked(ControlPackageConnection *connection, ControlPackageConnection *subConnection) { return; };
		void connectionUnlocked(ControlPackageConnection *connection, ControlPackageConnection *subConnection) { return; };
		void connectionClosing(ControlPackageConnection *connection, ControlPackageConnection *subConnection) { return; };

	private:
		void run();
		void handleControl(ExamplePackageMessage *msg);

		list<ExamplePackageMessage *> messages;

		bool alive;
		int dummy;		// dummy variable we'll manipulate with CONTROL messages
};


// ControlPackage Class Factories
extern "C" ControlPackage* create(ControlPackageCallback *callback) {
	ExamplePackage *pkg = new ExamplePackage();
	pkg->setCallback(callback);
	return pkg;
}

extern "C" void destroy(ControlPackage* p) {
	delete p;
}


// Class Methods
ExamplePackage::ExamplePackage() {
	MCMINIT();
	cout << "[XPKG] Registering ExamplePackage()" << endl;
	name = "msc-example-pkg";
	version = "1.0";
	desc = "Media Server Control - Example - Dummy - version 1.0";
	mimeType = "application/msc-example-pkg+xml";
	dummy = 0;
	alive = false;
}

ExamplePackage::~ExamplePackage() {
	cout << "[XPKG] Removing ExamplePackage()" << endl;
	if(alive) {
		alive = false;
		join();
	}
}

void ExamplePackage::setCollector(void *frameCollector)
{
	setCommonCollector(frameCollector);
	cout << "[XPGG] Frame Collector " << (getCollector() ? "OK" : "NOT OK :(") << endl;
}

bool ExamplePackage::setup()
{
	// Nothing to do here
	start();
	return true;
}

string ExamplePackage::getInfo()
{
	stringstream info;
	info << "Running: " << alive << endl;
	info << "Dummy = " << dec << dummy << endl;
	info << "Transactions:" << endl;
	if(messages.empty())
		info << "\tnone" << endl;
	else {
		list<ExamplePackageMessage *>::iterator iter;
		ExamplePackageMessage *msg = NULL;
		for(iter = messages.begin(); iter != messages.end(); iter++) {
			msg = (*iter);
			if(msg == NULL)
				continue;
			info << "\t" << msg->tid << endl;
		}
	}
	return info.str();
}

void ExamplePackage::control(void *sender, string tid, string blob) {
	cout << "[XPKG] \tExamplePackage() received CONTROL message (tid=" << tid << ")" << endl;
	ExamplePackageMessage *msg = new ExamplePackageMessage();
	msg->pkg = this;
	msg->tid = tid;
	msg->blob = blob;
	msg->len = blob.length();
	msg->level = 0;
	msg->sender = sender;
	messages.push_back(msg);
}

void ExamplePackage::run() {
	cout << "[XPKG] Joining ExamplePackage->thread()" << endl;
	bool waiting = false;
	alive = true;
	while(alive) {
		if(waiting) {	// Retry in 200ms
			struct timeval tv = {0, 200000};
			select(0, NULL, NULL, NULL, &tv);
		}
		if(messages.empty()) {
			waiting = true;
			continue;
		}
		waiting = false;
		ExamplePackageMessage *msg = NULL;
		while(!messages.empty()) {
			msg = messages.front();
			messages.pop_front();
			cout << "[XPKG] \tHandling CONTROL message (tid=" << msg->tid << ")" << endl;
			handleControl(msg);
			cout << "[XPKG] ExamplePackage->thread() back to sleep..." << endl;
		}
	}
	cout << "[XPKG] Leaving ExamplePackage->thread()" << endl;
}

void ExamplePackage::handleControl(ExamplePackageMessage *msg) {
	cout << "[XPKG] \tExamplePackage->control (tid=" << msg->tid << ")" << endl;
	if((msg->blob == "") && (msg->len > 0)) {
		cout << "[XPKG]     Invalid buffer (len = " << msg->len << ")" << endl;
		// TODO Error: invalid buffer
		delete msg;
		return;
	}
	if(msg->len == 0) {
		cout << "[XPKG]     Bodyless CONTROL" << endl;
		// TODO CONTROL without body?
		delete msg;
		return;
	}
	XML_Parser parser = XML_ParserCreate(NULL);
	XML_SetUserData(parser, msg);
	XML_SetElementHandler(parser, startElement, endElement);
	if (XML_Parse(parser, msg->blob.c_str(), msg->len, 1) == XML_STATUS_ERROR) {
		cout << "[XPKG]     Error parsing ExamplePackage CONTROL body: '"
			<< XML_ErrorString(XML_GetErrorCode(parser))
			<< "' at " << XML_GetCurrentLineNumber(parser) << ":"
			<< XML_GetCurrentColumnNumber(parser) << endl;
		delete msg;
		return;
	}
	XML_ParserFree(parser);
	delete msg;
}

void ExamplePackage::handleDummy(ExamplePackageMessage *msg, const char *op, const char *var) {
	if(!msg)
		return;	// FIXME
	stringstream blob;
	if(!op) {
		cout << "[XPKG]     Missing mandatory attribute 'op'" << endl;
		blob << "<?xml version=\"1.0\"?><example op=\"increment\" result=\"Error: missing mandatory attribute 'op'\"/>";
	} else if(!var) {
		cout << "[XPKG]     Missing mandatory attribute 'var'" << endl;
		blob << "<?xml version=\"1.0\"?><example op=\"increment\" result=\"Error: missing mandatory attribute 'var'\"/>";
	}
	if(strcasecmp(var, "dummy")) {
		// Error! We only manipulate dummy
		cout << "[XPKG]     Unknown variable '" << var << "'" << endl;
		blob << "<?xml version=\"1.0\"?><example op=\"increment\" result=\"Error: unknown variable '" << var << "'\"/>";
	}
	if(!strcasecmp(op, "increment")) {
		dummy++;
		cout << "[XPKG]     Incrementing 'dummy' --> " << dummy << endl;
		blob << "<?xml version=\"1.0\"?><example op=\"increment\" result=\"done\"/>";
	} else if(!strcasecmp(op, "decrement")) {
		dummy--;
		cout << "[XPKG]     Decrementing 'dummy' --> " << dummy << endl;
		blob << "<?xml version=\"1.0\"?><example op=\"decrement\" result=\"done\"/>";
	} else if(!strcasecmp(op, "read")) {
		cout << "[XPKG]     Accessing 'dummy' --> " << dummy << endl;
		blob << "<?xml version=\"1.0\"?><example op=\"read\" var=\"dummy\" value=\"" << dec << dummy << "\"/>";
	} else {
		// Error! Unrecognized operation
		cout << "[XPKG]     Unknown operation '" << op << "'" << endl;
		blob << "<?xml version=\"1.0\"?><example op=\"increment\" result=\"Error: unknown operation '" << op << "'\"/>";
	}
	if(dummy%2 == 0) {
		struct timeval tv = {25, 0};	// When dummy is even, we wait 25 seconds before answering... just a silly way to show how 202+REPORT works
		select(0, NULL, NULL, NULL, &tv);
	}
	callback->report(this, msg->sender, msg->tid, 200, 10, blob.str());
}


// eXpat callbacks handle the parsing of CONTROL bodies
static void XMLCALL startElement(void *msg, const char *name, const char **atts)
{
	ExamplePackageMessage *message = (ExamplePackageMessage *)msg;
	message->level++;
	if(message->level > 1)
		return;
	const char *op = NULL, *var = NULL;
	int i = 0;
	if(strcasecmp(name, "example")) {
		// <example> is the only allowed tag
		// TODO implement error handling
		return;
	}
	if(!atts) {
		// No attributes
		// TODO implement error handling
		return;
	}
	while(atts[i]) {
		if(!op && !strcasecmp(atts[i], "op")) {
			// op attribute, the operation
			op = atts[i+1];
		} else if(!var && !strcasecmp(atts[i], "var")) {
			// var attribute, the variable to manipulate
			var = atts[i+1];
		} else {
			// Unrecognized attribute
		}
		i = i+2;
	}
	message->pkg->handleDummy(message, op, var);
}

static void XMLCALL endElement(void *msg, const char *name)
{
	ExamplePackageMessage *message = (ExamplePackageMessage *)msg;
	message->level--;
}
