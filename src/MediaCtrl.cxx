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
 * \brief MediaCtrl Core (SIP+CFW, draft-ietf-mediactrl-sip-control-framework-10, draft-boulton-mmusic-sdp-control-package-attribute-03)
 *
 * \author Lorenzo Miniero <lorenzo.miniero@unina.it>
 *
 * \ingroup core
 * \ref core
 */

#include "MediaCtrl.h"
#include <dlfcn.h>
#include <dirent.h>
#include <boost/regex.hpp>

extern "C" {
#ifdef FFMPEG_ALTDIR
#include <libavcodec/avcodec.h>	// For FFmpeg initialization only
#else
#include <ffmpeg/avcodec.h>	// For FFmpeg initialization only
#endif
}

using namespace mediactrl;
using namespace boost;


// eXpat parser callbacks (for configuration)
static void XMLCALL startElement(void *msg, const char *name, const char **atts);
static void XMLCALL valueElement(void *msg, const XML_Char *s, int len);
static void XMLCALL endElement(void *msg, const char *name);


// MediaCtrl core class
MediaCtrl::MediaCtrl(string conf)
{
	cout << "*** MediaCtrl::MediaCtrl()" << endl;

	sipTransactions.clear();
	sipConnections.clear();
	sipHandlers.clear();
	endpointConnections.clear();
	endpointConferences.clear();
	codecs.clear();
	codecPlugins.clear();
	codecSharedObjects.clear();

	regex re;
	cmatch matches;

	configurationFile = conf;
	cout << "*** Using configuration file: " << configurationFile << endl;
	openConfiguration();
	string tmp = getConfValue("cfw", "address");
	cfwAddress = InetHostAddress((tmp != "" ? tmp.c_str() : NULL));
	tmp = getConfValue("cfw", "port");
	cfwPort = atoi((tmp != "" ? tmp.c_str() : "7575"));
	bool cfwKeepAlive = true;
	tmp = getConfValue("cfw", "force-kalive");
	if(tmp != "") {
		re.assign("true|yes|1", regex_constants::icase);	// FIXME Should be 'true' only?
		if(!regex_match(tmp.c_str(), re)) {
			re.assign("false|no|0", regex_constants::icase);	// FIXME Should be 'false' only?
			if(!regex_match(tmp.c_str(), re))
				cout << "Invalid value for 'force-kalive, defaulting to 'true'..." << endl;
			else
				cfwKeepAlive = false;
		} else
			cfwKeepAlive = true;
	}
	cout << "Tear down Dialog with a BYE if a Keep-Alive is not received in time? " << (cfwKeepAlive ? "YES" : "NO") << endl;
	tmp = getConfValue("cfw", "certificate");
	string tmp2 = getConfValue("cfw", "privatekey");
	tls = new TlsSetup();
	if(tls->setup(tmp, tmp2))	// FIXME Should check if it succeeds...
		cout << "TLS successfully setup" << endl;
	else
		cout << "Could not setup TLS..." << endl;
	cfwRestrict[0] = cfwRestrict[1] = cfwRestrict[2] = cfwRestrict[3] = 0;
	tmp = getConfValue("sip", "restrict");
	if(tmp != "") {
		cout << "Requested restriction on the IP range for SIP: " << tmp << endl;
		re.assign("(\\d+)\\.(\\d+)\\.(\\d+)\\.(\\d+)", regex_constants::icase);
		if(!regex_match(tmp.c_str(), matches, re)) {
			cfwRestrict[0] = cfwRestrict[1] = cfwRestrict[2] = cfwRestrict[3] = 0;
			cout << "     Invalid string, assuming 0.0.0.0" << endl;
		} else {
			tmp = matches[1];
			cfwRestrict[0] = atoi(tmp.c_str());
			tmp = matches[2];
			cfwRestrict[1] = atoi(tmp.c_str());
			tmp = matches[3];
			cfwRestrict[2] = atoi(tmp.c_str());
			tmp = matches[4];
			cfwRestrict[3] = atoi(tmp.c_str());
			if((cfwRestrict[0] > 255) || (cfwRestrict[1] > 255) || (cfwRestrict[2] > 255) || (cfwRestrict[3] > 255)) {
				cfwRestrict[0] = cfwRestrict[1] = cfwRestrict[2] = cfwRestrict[3] = 0;
				cout << "     Invalid string, assuming 0.0.0.0" << endl;
			}
			cout << "     Restriction enforced (" << dec << cfwRestrict[0] << "." << dec << cfwRestrict[1] << "." << dec << cfwRestrict[2] << "." << dec << cfwRestrict[3] << ")" << endl;
		}
	}

	tmp = getConfValue("sip", "address");
	sipAddress = InetHostAddress((tmp != "" ? tmp.c_str() : NULL));
	sipAddressString = tmp;
	tmp = getConfValue("sip", "port");
	sipPort = atoi((tmp != "" ? tmp.c_str() : "5060"));
	sipName = getConfValue("sip", "name");
	if(sipName == "")
		sipName = "MediaServer";
	tmp = getConfValue("monitor", "port");
	monitorPort = atoi((tmp != "" ? tmp.c_str() : "6789"));

//	// Initialize the reSIProcate SIP stack (FIXME)
	sip = new SipStack();
	dum = new DialogUsageManager(*sip);
	sipThread = new StackThread(*sip);
	dumThread = new DumThread(*dum);
//	sip->registerForTransactionTermination();

	// Initialize the oRTP stack
	rtpSetup();

	// Initialize the CFW stack (FIXME)
	cfw = new CfwStack(cfwAddress, cfwPort, cfwKeepAlive);
	cfw->setCfwManager(this);

	// Load all available codecs (as plugins)
	loadCodecs();
	cout << "*** List of loaded codecs:" << endl;
	map<int, CodecFactory*>::iterator iter;
	for(iter = codecs.begin(); iter != codecs.end(); iter++)
		cout << "     " << iter->second->getName() << endl;

	// Initialize the Remote Monitor interface
	monitor = new RemoteMonitor();
	monitor->setManager(this);
	assert(monitor->setPort(monitorPort) > -1);
	monitor->start();

	alive = true;
	acceptCalls = true;
}

MediaCtrl::~MediaCtrl()		// FIXME badly
{
	acceptCalls = false;
	cout << "*** MediaCtrl::~MediaCtrl()" << endl;

//	mSip.enter();
	if(!sipTransactions.empty()) {
		map<string, MediaCtrlSipTransaction *>::iterator iter;
		while(!sipTransactions.empty()) {
			MediaCtrlSipTransaction *t = sipTransactions.begin()->second;
			if(t == NULL) {	// FIXME
//				mSip.leave();
//				break;
				sipTransactions.erase(sipTransactions.begin());
				continue;
			}
			cout <<"[SIP] Terminated Call-ID: " << t->getCallId() << endl;
			if(t->isAS()) {	// This is a MediaCtrl SIP dialog from an MS, close the TCP connection as well
				cout << "[SIP] The SIP Dialog associated to the AS Control Channel was terminated" << endl;
				string cfwId = t->getCfwId();
				cout << "[SIP] Removing MediaCtrlClient " << cfwId << endl;
				cfw->removeClient(cfwId);
			}
			sipTransactions.erase(t->getCallId());
			sipConnections.erase(t->getConnectionId());

			t->sendBye();

			delete t;
		}
	}
//	mSip.leave();

	struct timeval tv = {2, 0};
	select(0, NULL, NULL, NULL, &tv);

	if(alive) {
		alive = false;
		join();
	}

	rtpCleanup();

	// Get rid of connection and conference endpoints
	if(!endpointConnections.empty()) {
		map<string, MediaCtrlConnection*>::iterator iter;
		while(!endpointConnections.empty()) {
			iter = endpointConnections.begin();
			MediaCtrlConnection *connection = iter->second;
			if(connection) {
				cout << "[SIP]\t\tDestroying Connection: " << connection->getId() << endl;
				delete connection;
			}
			endpointConnections.erase(iter);
		}
	}
	if(!endpointConferences.empty()) {
		map<string, MediaCtrlConference*>::iterator iter;
		while(!endpointConferences.empty()) {
			iter = endpointConferences.begin();
			MediaCtrlConference *conference = iter->second;
			if(conference) {
				cout << "[SIP]\t\tDestroying Conference: " << conference->getId() << endl;
				delete conference;
			}
			endpointConferences.erase(iter);
		}
	}

	delete cfw;
	delete monitor;

	// Finally destroy the codecs (the shared one, which may have been needed by packages)
	if(!codecPlugins.empty()) {
		map<int, MediaCtrlCodec*>::iterator iter;
		while(!codecPlugins.empty()) {
			iter = codecPlugins.begin();
			MediaCtrlCodec *codec = iter->second;
			cout << "[SIP]\t\tDestroying Codec: " << codec->getName() << endl;
			CodecFactory *factory = codecs[codec->getCodecId()];
			if(factory) {
//				codecs[codec->getCodecId()] = NULL;
				codecs.erase(codec->getCodecId());
				factory->destroy(codec);
			}
			codecPlugins.erase(iter);
		}
	}
	if(!codecs.empty()) {
		map<int, CodecFactory*>::iterator iter;
		while(!codecs.empty()) {
			iter = codecs.begin();
			CodecFactory *factory = iter->second;
			if(factory) {
				factory->purge();
				delete factory;
			}
			codecs.erase(iter);
		}
	}
	if(!codecSharedObjects.empty()) {
		void *plugin = NULL;
		while(!codecSharedObjects.empty()) {
			plugin = codecSharedObjects.front();
			codecSharedObjects.pop_front();
			dlclose(plugin);
		}
	}
}

int MediaCtrl::openConfiguration()
{
	FILE *f = fopen(configurationFile.c_str(), "rb");
	if(!f) {
		cout << "[CONF]     Error opening configuration file..." << endl;
		return -1;
	}
	char buffer[100];
	memset(buffer, 0, 100);
	configuration = "";
	while(fgets(buffer, 100, f) != NULL) {
		configuration += buffer;
		memset(buffer, 0, 100);
	}
	fclose(f);
	cout << "[CONF]     Opened configuration file:" << endl;
	cout << configuration << endl;
	// Only scan the XML blob for errors...
	XML_Parser parser = XML_ParserCreate(NULL);
	MediaCtrlConfValue *confValue = new MediaCtrlConfValue("");
	XML_SetUserData(parser, confValue);
	XML_SetElementHandler(parser, startElement, endElement);
	XML_SetCharacterDataHandler(parser, valueElement);
	if (XML_Parse(parser, configuration.c_str(), configuration.length(), 1) == XML_STATUS_ERROR) {
		cout << "[CONF]     Error parsing configuration file: '"
			<< XML_ErrorString(XML_GetErrorCode(parser))
			<< "' at " << XML_GetCurrentLineNumber(parser) << ":"
			<< XML_GetCurrentColumnNumber(parser) << endl;
		return -1;
	}
	cout << "[CONF]     Configuration file looks fine" << endl;
	XML_ParserFree(parser);
	delete confValue;
	return 0;
}

string MediaCtrl::getConfValue(string element, string attribute)
{
	if(element == "")
		return "";
	XML_Parser parser = XML_ParserCreate(NULL);
	MediaCtrlConfValue *confValue = new MediaCtrlConfValue(element, attribute);
	XML_SetUserData(parser, confValue);
	XML_SetElementHandler(parser, startElement, endElement);
	XML_SetCharacterDataHandler(parser, valueElement);
	if (XML_Parse(parser, configuration.c_str(), configuration.length(), 1) == XML_STATUS_ERROR) {
		cout << "[CONF]     Error parsing configuration file: '"
			<< XML_ErrorString(XML_GetErrorCode(parser))
			<< "' at " << XML_GetCurrentLineNumber(parser) << ":"
			<< XML_GetCurrentColumnNumber(parser) << endl;
		return "";
	}
	XML_ParserFree(parser);
	string result = confValue->getResult();
	delete confValue;
	return result;
}

string MediaCtrl::getPackageConfValue(string package, string element, string attribute)
{
	if((package == "") || (element == ""))
		return "";
	XML_Parser parser = XML_ParserCreate(NULL);
	MediaCtrlConfValue *confValue = new MediaCtrlConfValue(element, attribute);
	confValue->setPackage(package);
	XML_SetUserData(parser, confValue);
	XML_SetElementHandler(parser, startElement, endElement);
	XML_SetCharacterDataHandler(parser, valueElement);
	if (XML_Parse(parser, configuration.c_str(), configuration.length(), 1) == XML_STATUS_ERROR) {
		cout << "[CONF]     Error parsing configuration file: '"
			<< XML_ErrorString(XML_GetErrorCode(parser))
			<< "' at " << XML_GetCurrentLineNumber(parser) << ":"
			<< XML_GetCurrentColumnNumber(parser) << endl;
		return "";
	}
	XML_ParserFree(parser);
	string result = confValue->getResult();
	delete confValue;
	return result;
}

void MediaCtrl::loadCodecs()
{
	cout << "[SIP] Initializing libavcodec" << endl;
	avcodec_init();
	avcodec_register_all();
	DIR *dir = NULL;
	struct dirent *plugin = NULL;
	// Take all shared objects and try to load them
	string path = getConfValue("codecs", "path");
	if(path == "")
		path = "./codecs";
	cout << "[SIP] Codecs folder: " << path << endl;
	dir = opendir(path.c_str());	// FIXME
	if(!dir) {
		cout << "[SIP] Couldn't access 'codecs' folder! No plugins will be used..." << endl;
		return;
	}
	char pluginpath[255];
	while((plugin = readdir(dir))) {
		int len = strlen(plugin->d_name);
		if (len < 4)
			continue;
		if (strcasecmp(plugin->d_name+len-3, ".so"))
			continue;
		cout << "[SIP] Loading plugin '" << plugin->d_name << "'..." << endl;
		memset(pluginpath, 0, 255);
		sprintf(pluginpath, "%s/%s", path.c_str(), plugin->d_name);
		void *codecPlugin = dlopen(pluginpath, RTLD_LAZY);
		if (!codecPlugin)
			cout << "[SIP]     Couldn't load plugin '" << plugin->d_name << "': " << dlerror() << endl;
		else {
			create_cd *create_c = (create_cd*) dlsym(codecPlugin, "create");
			const char *dlsym_error = dlerror();
			if (dlsym_error) {
				cout << "[SIP]     Couldn't load symbol 'create': " << dlsym_error << endl;
				continue;
			}
			destroy_cd *destroy_c = (destroy_cd*) dlsym(codecPlugin, "destroy");
			dlsym_error = dlerror();
			if (dlsym_error) {
				cout << "[SIP]     Couldn't load symbol 'destroy': " << dlsym_error << endl;
				return;
			}
			purge_cd *purge_c = (purge_cd*) dlsym(codecPlugin, "purge");
			dlsym_error = dlerror();
			if (dlsym_error) {
				cout << "[SIP]     Couldn't load symbol 'purge': " << dlsym_error << endl;
				return;
			}
			MediaCtrlCodec* newcodec = create_c();
			if(!newcodec) {
				cout << "[SIP]     Couldn't create the new codec instance..." << endl;
				continue;
			}
			if(getCollector())
				cout << "[SIP]     Setting valid collector" << endl;
			else
				cout << "[SIP]     Setting INVALID collector" << endl;
			newcodec->setCollector(getCollector());
			cout << "[SIP]     Testing codec startup..." << endl;
			if(newcodec->start() == false)
				cout << "[SIP]         FAILURE" << endl;
			else {
				cout << "[SIP]         SUCCESS" << endl;
				int codec = newcodec->getCodecId();
				string name = newcodec->getName();
				string nameMask = newcodec->getNameMask();
				int blockLen = newcodec->getBlockLen();
				cout << "[SIP] Adding codec ID " << codec << " (" << name << ") to the map" << endl;
				codecs[codec] = new CodecFactory(name, nameMask, create_c, destroy_c, purge_c, blockLen);
				if(newcodec->getMediaType() == MEDIACTRL_MEDIA_AUDIO)
					codecPlugins[codec] = newcodec;
				else {
					cout << "[SIP]     Removing test codec..." << endl;
					destroy_c(newcodec);
				}
				codecSharedObjects.push_back(codecPlugin);
			}
		}
	}
	closedir(dir);
}

MediaCtrlCodec *MediaCtrl::createCodec(int codec)
{
	// Try to create the new codec
	if(codec == 122)
		codec = 99;	// FIXME Dirty hack to handle 122 (H.264 for Ekiga) as 99 (H.264 for Grandstream)
	if(codecs[codec] == NULL)
		codec = 200;	// FIXME handle dynamic payload types
	MediaCtrlCodec *newcodec = codecs[codec]->create();
	if(!newcodec) {
		cout << "[SIP] Couldn't create the new codec instance..." << endl;
		return NULL;
	}
/*	cout << "[SIP] Codec startup..." << endl;
	if(newcodec->start() == false) {
		cout << "[SIP]         FAILURE" << endl;
		codecs[codec]->destroy(newcodec);
		return NULL;
	}
	cout << "[SIP]         SUCCESS" << endl;*/
	newcodec->setCollector(getCollector());
	int codecId = newcodec->getCodecId();
	string name = newcodec->getName();
	cout << "[SIP] Opened codec ID " << codecId << " (" << name << ")" << endl;

	return newcodec;
}

int MediaCtrl::getBlockLen(int codec)
{
	if(codecs[codec] == NULL)
		return -1;

	return codecs[codec]->getBlockLen();
}

MediaCtrlEndpoint *MediaCtrl::getEndpoint(ControlPackage *cp, string conId)
{
	// First of all split connection-id/conf-id
	regex re;
	cmatch matches;
	re.assign("((\\w|\\-)+)|(((\\w|\\-)+)~((\\w|\\-)+))", regex_constants::icase);
	if(!regex_match(conId.c_str(), matches, re)) {
		cout << "[SIP] connection-id/conf-id is in invalid format" << endl;
		return NULL;
	}
	string Id = "", label = "";
	int what = 0;
	if(matches[1].first != matches[1].second) {		// First match: conference
		what = 1;
		Id = matches[1];
	} else if(matches[3].first != matches[3].second) {	// Second match: connection-id
		what = 2;
		Id = matches[3];
	}

	cout << "[SIP] Request is for:" << endl;
	cout << "[SIP]\t\tId=" << Id << (what == 1 ? " (conference)" : " (connection)") << endl;

	if(what == 1) {	// Look in conferences
		MediaCtrlConference *conference = endpointConferences[Id];
		if(conference != NULL)
			cout << "[SIP] Found endpoint in conferences: " << Id << endl;
		return conference;
	} else {	// Look in connections
		MediaCtrlSipTransaction *sipTransaction = sipConnections[Id];
		if(sipTransaction == NULL)
			return NULL;
		MediaCtrlConnection *endpoint = endpointConnections[Id];
		if(endpoint == NULL) {		// Create a new wrapper
			endpoint = new MediaCtrlConnection(Id);
			cout << "[SIP] Wrapper created" << endl;
			// Add all labels
			list<string>labels = sipTransaction->getMediaLabels();
			if(!labels.empty()) {
				list<string>::iterator iter;
				for(iter = labels.begin(); iter != labels.end(); iter++) {
					string newlabel = (*iter);
					cout << "[SIP] \tCreating individual endpoint connection: " << newlabel << endl;
					MediaCtrlConnection *endpointByLabel = new MediaCtrlConnection(Id, newlabel);
					if(!endpointByLabel)
						continue;	// FIXME
					cout << "[SIP] \t\tIndividual endpoint creation created, wrapping it...";
					if(endpointByLabel->setOwner(endpoint) < 0)
						cout << "Failure...";
					else
						cout << "OK...";
					if(endpoint->addOwned(endpointByLabel) < 0)
						cout << "Failure...";
					else
						cout << "OK...";
					cout << endl;
					endpointByLabel->setLocal(sipTransaction, sipTransaction->getRtpChannel(newlabel));	// FIXME
					sipTransaction->setSipManager(endpointByLabel, newlabel);
				}
			}
			endpointConnections[Id] = endpoint;
		}
		endpoint->increaseCounter();
		return endpoint;
	}
}

MediaCtrlEndpoint *MediaCtrl::createConference(ControlPackage *cp, string confId)
{
	if(confId == "")
		return NULL;
	// Look for connection/conference, if it exists
	MediaCtrlSipTransaction *sipTransaction = sipConnections[confId];
	if(sipTransaction != NULL)
		return NULL;
	MediaCtrlConference *conference = endpointConferences[confId];
	if(conference != NULL)
		return NULL;

	conference = new MediaCtrlConference(confId);
	endpointConferences[confId] = conference;

	return conference;
}

MediaCtrlFrame *MediaCtrl::decode(MediaCtrlFrame *frame)
{
	if(!frame)
		return NULL;
	int pt = frame->getFormat();
	if(pt == MEDIACTRL_RAW)		// Already decoded
		return frame;
	MediaCtrlCodec *decoder = codecPlugins[pt];
	if(!decoder)
		return NULL;
	return decoder->decode(frame);
}

MediaCtrlFrame *MediaCtrl::encode(MediaCtrlFrame *frame, int dstFormat)
{
	if(!frame)
		return NULL;
	int pt = frame->getFormat();
	if(pt == dstFormat)		// Already in the right format
		return frame;
	MediaCtrlFrame *decoded = frame;
	if(pt != MEDIACTRL_RAW) {	// We need raw frames
		decoded = decode(frame);
		if(!decoded)
			return NULL;
	}
	MediaCtrlCodec *encoder = codecPlugins[dstFormat];
	if(!encoder)
		return NULL;
	return encoder->encode(decoded);
}

void MediaCtrl::endDialog(string callId)
{
	cout << "[SIP] The CFW stack requested to end a SIP dialog: " << callId << endl;
	MediaCtrlSipTransaction *t = sipTransactions[callId];
	if(t) {
		if(t->isAS()) {	// This is a MediaCtrl SIP dialog from an MS, close the TCP connection as well
			cout << "[SIP] The SIP Dialog associated to the AS Control Channel was terminated" << endl;
			string cfwId = t->getCfwId();
			cout << "[SIP] Removing MediaCtrlClient " << cfwId << endl;
			cfw->removeClient(cfwId);
		}
		sipTransactions.erase(t->getCallId());
		sipConnections.erase(t->getConnectionId());
		t->sendBye();
	} else
		cout << "[SIP]\t\tNo such a Call-Id..." << endl;
}


void MediaCtrl::thread()
{
	cout << "*** MediaCtrl::thread()" << endl;
	cfw->run();
	contact.uri().scheme() = "sip";
	contact.uri().user() = sipName.c_str();
	contact.uri().host() = sipAddressString.c_str();		// !!! FIXME !!!
	contact.uri().port() = sipPort;
	contact.uri().param(p_transport) = Tuple::toData(UDP);
	SharedPtr<MasterProfile> profile(new MasterProfile);
	profile->setDefaultFrom(contact);
	profile->addSupportedMethod(INVITE);
	profile->addSupportedMethod(REGISTER);
	dum->setMasterProfile(profile);
	dum->setInviteSessionHandler(this);
	dum->setServerRegistrationHandler(this);
	InMemoryRegistrationDatabase regData;
	dum->setRegistrationPersistenceManager(&regData);
	// Start the SIP server too
	dum->addTransport(UDP, sipPort, V4);

	sipThread->run();
	dumThread->run();
	struct timeval tv;
	while(alive) {
		tv.tv_sec = 1;
		tv.tv_usec = 0;
		select(0, NULL, NULL, NULL, &tv);
	}

	sipThread->shutdown();
	dumThread->shutdown();
	sipThread->join();
	dumThread->join();
	delete sipThread;
	delete dumThread;
	delete dum;
	delete sip;

	cout << "[SIP] Leaving thread..." << endl;
}

void MediaCtrl::onNewSession(ServerInviteSessionHandle sis, InviteSession::OfferAnswerType oat, const SipMessage& msg)
{
	cout << ": ServerInviteSession-onNewSession - " << msg.brief() << endl;

	if(!acceptCalls) {
		cout << "[SIP] Rejecting the call, since we're cleaning up..." << endl;
		sis->reject(404);	// FIXME
		return;
	}

	string to = msg.header(h_RequestLine).uri().user().c_str();
	cout << "[SIP] Requested URI: " << to << endl;
	// Match URI in Request-Line
	if(to != sipName) {
		cout << "[SIP]\tInvalid: should be " << sipName << endl;
		sis->reject(404);	// FIXME
		return;
//		cout << "[SIP]\t\tBut I'll accept the INVITE anyway..." << endl;
	}

	string callId = msg.header(h_CallID).value().c_str();
	MediaCtrlSipTransaction *newt = new MediaCtrlSipTransaction(callId, sis);
//	mSip.enter();
	sipTransactions[callId] = newt;
//	mSip.leave();
	newt->setCodecManager(this);
}

void MediaCtrl::onOffer(InviteSessionHandle is, const SipMessage& msg, const SdpContents& sdp)
{
	cout << ": InviteSession-onOffer(SDP)" << endl;

	MediaCtrlSipTransaction *t = sipTransactions[msg.header(h_CallID).value().c_str()];
	if(t == NULL) {	// FIXME
		return;
	}

	if(t->getNegotiated() == true)	// FIXME reINVITE or retransmit?
		return;
	t->setNegotiated();
	t->setInviteHandler(is);

	ServerInviteSessionHandle sis = t->getHandler();

	// First of all check if there's any restriction
	string ip = msg.header(h_Contacts).front().uri().host().c_str();
	cout << "[SIP] Host is " << ip << endl;	// FIXME
	if((cfwRestrict[0] > 0) || (cfwRestrict[1] > 0) || (cfwRestrict[2] > 0) || (cfwRestrict[3] > 0)) {
		struct sockaddr_in address;
		uint16_t digits[4];
		digits[0] = digits[1] = digits[2] = digits[3] = 0;
		if(inet_aton(ip.c_str(), (in_addr *)&address) != 1) {		// Resolve the address first
			struct hostent *host = gethostbyname(ip.c_str());
			if(host == NULL) {
				cout << "[SIP]\tInvalid host" << endl;
				sis->reject(403);	// FIXME
				sipTransactions.erase(t->getCallId());
				sipConnections.erase(t->getConnectionId());
				delete t;
				return;
			}
			address.sin_addr.s_addr = *(uint32_t *)(host->h_addr_list[0]);
			ip = inet_ntoa(address.sin_addr);
		}
		regex re;
		cmatch matches;
		re.assign("(\\d+)\\.(\\d+)\\.(\\d+)\\.(\\d+)", regex_constants::icase);
		if(!regex_match(ip.c_str(), matches, re)) {
			cout << "[SIP]\tInvalid host" << endl;
			sis->reject(403);	// FIXME
			sipTransactions.erase(t->getCallId());
			sipConnections.erase(t->getConnectionId());
			delete t;
			return;
		}
		string tmp = matches[1];
		digits[0] = atoi(tmp.c_str());
		tmp = matches[2];
		digits[1] = atoi(tmp.c_str());
		tmp = matches[3];
		digits[2] = atoi(tmp.c_str());
		tmp = matches[4];
		digits[3] = atoi(tmp.c_str());
		// Check if the restriction is respected
		int ok = 0;
		if((cfwRestrict[0] == 0) || (digits[0] == cfwRestrict[0]))
			ok++;
		if((cfwRestrict[1] == 0) || (digits[1] == cfwRestrict[1]))
			ok++;
		if((cfwRestrict[2] == 0) || (digits[2] == cfwRestrict[2]))
			ok++;
		if((cfwRestrict[3] == 0) || (digits[3] == cfwRestrict[3]))
			ok++;
		if(ok != 4) {
			cout << "[SIP]\tRejected host " << ip << " because of restriction" << endl;
			sis->reject(403);	// FIXME
			sipTransactions.erase(t->getCallId());
			sipConnections.erase(t->getConnectionId());
			delete t;
			return;
		}
		cout << "[SIP]\tRestriction enforced (" << dec << cfwRestrict[0] << "." << dec << cfwRestrict[1] << "." << dec << cfwRestrict[2] << "." << dec << cfwRestrict[3] << ")" << endl;
	}

	sis->provisional(100, false);

	list<SdpContents::Session::Medium>media;	// List of media to match the offer

	Token req;
	StringCategory cp;
	bool is_cfw = false;
	string callId = msg.header(h_CallID).value().c_str();
	string toTag = sis->getAppDialog()->getDialogId().getLocalTag().c_str();
	string fromTag = msg.header(h_From).param(p_tag).c_str();
	string cfwId = "";

	t->setTags(fromTag, toTag);
	sipConnections[t->getConnectionId()] = t;

	// Parse SDP
	ip = "";
	uint16_t port = 0;
	cout << "[SIP]   Parsing SDP..." << endl;
	Data tmp = sdp.getBodyData();
	cout << tmp << endl;
	int err = 200;
	string fingerprint;
	cout << "[SIP]     Handling media negotiation..." << endl;
	for (list<SdpContents::Session::Medium>::const_iterator i = sdp.session().media().begin(); i != sdp.session().media().end(); i++) {
		if(err != 200)
			break;	// Error handling the offer (FIXME)
		cout << "[SIP]       New " << i->name() << endl;
		if(i->name() == "application") {
			if((i->protocol() == "TCP/CFW") || (i->protocol() == "TCP/TLS/CFW")){
				ip = i->getConnections().front().getAddress().c_str();
				port = i->port();
				cout << "[SIP]       COMEDIA negotiation of the CFW connection establishment from " << ip << ":" << port << endl;
				cout << "[SIP]           Setup: " << i->getValues("setup").front() << endl;
				cout << "[SIP]           Connection: " << i->getValues("connection").front() << endl;
				cout << "[SIP]           Protocol: " << i->protocol() << endl;
				if(i->protocol() == "TCP/TLS/CFW") {
					if(i->getValues("fingerprint").front() == "") {
						cout << "[SIP]              Fingerprint: MISSING! (we'll reject the call)" << endl;
						err = 403;	// FIXME
						continue;
					}
					regex re;
					cmatch matches;
					re.assign("(\\S+) (\\S+)", regex_constants::icase);
					if(!regex_match(i->getValues("fingerprint").front().c_str(), matches, re)) {
						cout << "[SIP]              Fingerprint: BAD FORMAT! (we'll reject the call)" << endl;
						err = 403;	// FIXME
						continue;
					}
					string hashFunc = string(matches[1].first, matches[1].second);
					fingerprint = string(matches[2].first, matches[2].second);
					cout << "[SIP]              Fingerprint: --" << hashFunc << "-- " << fingerprint << endl;
					if(hashFunc != "SHA-1") {
						cout << "[SIP]              Hash function is not SHA-1! (we'll reject the call)" << endl;
						err = 403;	// FIXME
						continue;
					}
				}
				list<Data>cfwIds = i->getValues("cfw-id");
				if(cfwIds.size() != 1) {
					// TODO Handle error
					cout << "[SIP] There's not a single cfw-id, expect problems..." << endl;
				} else {
					cfwId = cfwIds.front().c_str();
					cout << "[SIP]           Dialog-ID: " << cfwId << endl;
				}
				list<Data>ctrlPackages = i->getValues("ctrl-package");
				if(!ctrlPackages.empty()) {
					cout << "[SIP]           Control Packages:" << endl;
					list<Data>::iterator iter;
					for(iter = ctrlPackages.begin(); iter != ctrlPackages.end(); iter++)
						cout << "[SIP]                       " << (*iter) << endl;
				}
				is_cfw = true;
				t->setAS(cfwId);
				// Add new MediaCtrlClient (take values from message)
				if(i->protocol() == "TCP/TLS/CFW")
					err = cfw->addClient(callId, cfwId, ip, port, true, fingerprint);
				else
					err = cfw->addClient(callId, cfwId, ip, port);
				if(err == 200) {
					SdpContents::Session::Medium medium("application", cfw->getPort(), 0, i->protocol());
					medium.addFormat("*");	// FIXME
					medium.addAttribute("connection", "new");
					medium.addAttribute("setup", "passive");
					if(i->protocol() == "TCP/TLS/CFW") {
						stringstream fingerprint;
						fingerprint << "SHA-1 " << tls->getFingerprint();
						medium.addAttribute("fingerprint", fingerprint.str().c_str());
					}
					medium.addAttribute("cfw-id", cfwId.c_str());
					list<string> pkgs = cfw->getPackages();
					list<string>::iterator iter;
					string pkg;
					for(iter = pkgs.begin(); iter != pkgs.end(); iter++) {
						pkg = (*iter);
						cout << "[SIP]       Adding " << pkg << " to the SDP" << endl;
						medium.addAttribute("ctrl-package", pkg.c_str());
					}
					media.push_back(medium);
				}
			} else {
				cout << "[SIP]          We don't support " << i->name() << " " << i->protocol() << endl;
				SdpContents::Session::Medium medium(i->name(), 0, 0, i->protocol());
				media.push_back(medium);
			}
		} else if(i->name() == "video") {
			// We don't support video in this version, reject the media
			cout << "[SIP]          We don't support " << i->name() << endl;
			SdpContents::Session::Medium medium(i->name(), 0, 0, i->protocol());
			media.push_back(medium);
		} else if(i->name() == "audio") {
			if(t->rtpExists(ip, i->port())) {	// TODO Update media
				cout << "[SIP]          Should update " << i->name() << "..." << endl;
			} else {	// Match the offer, and create a new RTP connection
				int type = MEDIACTRL_MEDIA_AUDIO;
				ip = i->getConnections().front().getAddress().c_str();
				SdpContents::Session::Medium medium(i->name(), 0, 0, i->protocol());
				uint16_t rtpPort = 0;
				list<Data>formats = i->getFormats();
				for (list<Data>::const_iterator j = formats.begin(); j != formats.end(); j++) {
					int jj = atoi((*j).c_str());
					if(jj == 101)
						continue;
					// Get the codec name
					list<Data>values = i->getValues("rtpmap");
					regex re;
					cmatch matches;
					bool rtpmapFound = false;
					for (list<Data>::const_iterator k = values.begin(); k != values.end(); k++) {
						re.assign("(\\d+) ((\\w|\\-)+)/(\\d+)", regex_constants::icase);
						if(!regex_match((*k).c_str(), matches, re))
							continue;
						if(atoi(matches[1].first) == jj) {
							rtpmapFound = true;
							break;
						}
					}
					if(rtpmapFound)
						cout << "[SIP]         Matching AVT " << matches[1] << " " << matches[2] << "/" << matches[4] << "... ";
					else
						cout << "[SIP]         Matching AVT " << dec << jj << " (no rtpmap)... ";
					if(codecExists(jj)) {		// Static Payload Type
						list<string> attributes;
						attributes.clear();
						cout << "OK" << endl;
						if(!rtpPort) {
							rtpPort = t->addRtp(jj, type);
							// Get all the fmtp attributes associated with this codec, if any
							list<Data>fmtps = i->getValues("fmtp");
							if(!fmtps.empty()) {
								cout << "[SIP] Found " << dec << fmtps.size() << " fmtp attributes" << endl;
								regex fre;
								cmatch fmatches;
								for (list<Data>::const_iterator kk = fmtps.begin(); kk != fmtps.end(); kk++) {
									// We need to check for spaces as well, since X-lite doesn't conform to the semicolon separators standard
									fre.assign("(\\d+) ((\\S| )+)", regex_constants::icase);
									if(!regex_match((*kk).c_str(), fmatches, fre))
										continue;
									if(atoi(fmatches[1].first) == jj) {
										cout << "[SIP]     Parsing " << (*kk) << endl;
										string answer = t->addRtpSetting(rtpPort, string(fmatches[2].first, fmatches[2].second));
										if(answer != "") {
											stringstream newattribute;
											newattribute << dec << jj << " " << answer;
											attributes.push_back(newattribute.str());
										}
									}
								}
							}
							medium.setPort(rtpPort);
							if(t->setRtpPeer(rtpPort, ip.c_str(), i->port()) == false)
								cout << "[SIP]           Couldn't set RTP peer for " << i->name() << " (:" << rtpPort << " / " << ip << ":" << i->port() << ")" << endl;
							else
								cout << "[SIP]           RTP peer for " << i->name() << " has been set (:" << rtpPort << " / " << ip << ":" << i->port() << ")" << endl;
						}
						if(!rtpmapFound) {
							// No rtpmap, add the format manually
							char supportedFormat[3];
							sprintf(supportedFormat, "%d", jj);
							medium.addFormat(supportedFormat);
						} else {
							// Answer building the rtpmap line as the UAC did
							string name(matches[2].first, matches[2].second);
							string avt(matches[1].first, matches[1].second);
							string clock(matches[4].first, matches[4].second);
							SdpContents::Session::Codec newCodec(name.c_str(), atoi(avt.c_str()), atoi(clock.c_str()));
							medium.addCodec(newCodec);
						}
						if(!attributes.empty()) {
							while(!attributes.empty()) {
								string att = attributes.front();
								attributes.pop_front();
								medium.addAttribute("fmtp", att.c_str());
							}
						}
					} else {	// Dynamic payload type or unsupported? Check if we support it by matching the codec name
						cout << "not found... ";
						cout << "looking in codecs... ";
						map<int, CodecFactory*>::iterator iter;
						bool found = false;
						for(iter = codecs.begin(); iter != codecs.end(); iter++) {
							if(found)
								break;
							if(iter->second == NULL)
								continue;
							if(iter->second->checkName(matches[2])) {		// FIXME
								list<string> attributes;
								attributes.clear();
								cout << "found " << matches[2] << ", OK" << endl;
								found = true;
								if(!rtpPort) {
									rtpPort = t->addRtp(jj, type);	// FIXME Dynamic payload, need another way to add new RTP
									// Get all the fmtp attributes associated with this codec, if any
									list<Data>fmtps = i->getValues("fmtp");
									if(!fmtps.empty()) {
										cout << "[SIP] Found " << dec << fmtps.size() << " fmtp attributes" << endl;
										regex fre;
										cmatch fmatches;
										for (list<Data>::const_iterator kk = fmtps.begin(); kk != fmtps.end(); kk++) {
											// We need to check for spaces as well, since X-lite doesn't conform to the semicolon separators standard
											fre.assign("(\\d+) ((\\S| )+)", regex_constants::icase);
											if(!regex_match((*kk).c_str(), fmatches, fre))
												continue;
											if(atoi(fmatches[1].first) == jj) {
												cout << "[SIP]     Parsing " << (*kk) << endl;
												string answer = t->addRtpSetting(rtpPort, string(fmatches[2].first, fmatches[2].second));
												if(answer != "") {
													stringstream newattribute;
													newattribute << dec << jj << " " << answer;
													attributes.push_back(newattribute.str());
												}
											}
										}
									}
									medium.setPort(rtpPort);
									if(t->setRtpPeer(rtpPort, ip.c_str(), i->port()) == false)
										cout << "[SIP]          Couldn't set RTP peer for " << i->name() << " (:" << rtpPort << " / " << ip << ":" << i->port() << ")" << endl;
									else
										cout << "[SIP]          RTP peer for " << i->name() << " has been set (:" << rtpPort << " / " << ip << ":" << i->port() << ")" << endl;
								}
								if(!rtpmapFound) {
									// No rtpmap, add the format manually
									char supportedFormat[3];
									sprintf(supportedFormat, "%d", jj);
									medium.addFormat(supportedFormat);
								} else {
									// Answer building the rtpmap line as the UAC did
									string name(matches[2].first, matches[2].second);
									string avt(matches[1].first, matches[1].second);
									string clock(matches[4].first, matches[4].second);
									SdpContents::Session::Codec newCodec(name.c_str(), atoi(avt.c_str()), atoi(clock.c_str()));
									medium.addCodec(newCodec);
								}
								if(!attributes.empty()) {
									while(!attributes.empty()) {
										string att = attributes.front();
										attributes.pop_front();
										medium.addAttribute("fmtp", att.c_str());
									}
								}
							}
						}
						if(!found)
							cout << "not found" << endl;
					}
				}
				if(rtpPort) {
					if(type == MEDIACTRL_MEDIA_AUDIO) {
						medium.addAttribute("ptime", "20");
						if(i->findTelephoneEventPayloadType() > 0) {
							medium.addCodec(SdpContents::Session::Codec::TelephoneEvent);
							medium.addAttribute("fmtp", "101 0-15");
						}
					}
					medium.addAttribute("label", (Data)t->getMediaLabel(rtpPort));
					string label = t->getMediaLabel(rtpPort);
					cout << "[SIP]          label=" << label << " (" << t->getConnectionId() << " --> " << t->getConnectionId(label) << ")" << endl;
				} else
					cout << "[SIP]          Apparently we don't support any of the provided " << i->name() << " formats" << endl;
				media.push_back(medium);
			}
		} else {	// Any other protocol that may be specified, we don't support it
			cout << "[SIP]          We don't support " << i->name() << endl;
			SdpContents::Session::Medium medium(i->name(), 0, 0, i->protocol());
			media.push_back(medium);
		}
	}
	if(err != 200) {
		sis->reject(err);	// FIXME
		sipTransactions.erase(t->getCallId());
		sipConnections.erase(t->getConnectionId());
		delete t;
		return;
	}

	// Set us as handlers of all the media, for the moment
	t->setSipManager(this);

	// TODO Fix the SDP headers
	SdpContents sdpMS;
	Data address(sipAddressString);		// !!! FIXME !!!
	char *login = getlogin();
	SdpContents::Session::Origin origin(login ? login : "-", sdp.session().origin().getSessionId(), sdp.session().origin().getVersion()+1, SdpContents::IP4, address);
	SdpContents::Session session(0, origin, "MediaCtrl");
	session.connection() = SdpContents::Session::Connection(SdpContents::IP4, address);
	session.addTime(SdpContents::Session::Time(0, 0));
	// Add all the negotiated media to the answer
	list<SdpContents::Session::Medium>::iterator iter;
	for(iter = media.begin(); iter != media.end(); iter++ )
		session.addMedium((*iter));
	sdpMS.session() = session;
	// Accept the offer
	is->provideAnswer(sdpMS);
	sis->accept();
}

void MediaCtrl::onConnected(InviteSessionHandle is, const SipMessage& msg)
{
	cout << ": InviteSession-onConnected()" << endl;
}

void MediaCtrl::onTerminated(InviteSessionHandle is, InviteSessionHandler::TerminatedReason reason, const SipMessage* msg)
{
//	mSip.enter();
	cout << "InviteSessionHandler::onTerminated" << endl;
	if(msg) {
		cout <<"[SIP] Terminated Call-ID: " << msg->header(h_CallID).value().c_str() << endl;
		MediaCtrlSipTransaction *t = sipTransactions[msg->header(h_CallID).value().c_str()];
		if(t == NULL) {	// FIXME
//			mSip.leave();
			return;
		}
		if(t->isAS()) {	// This is a MediaCtrl SIP dialog from an MS, close the TCP connection as well
			cout << "[SIP] The SIP Dialog associated to the AS Control Channel was terminated" << endl;
			string cfwId = t->getCfwId();
			cout << "[SIP] Removing MediaCtrlClient " << cfwId << endl;
			cfw->removeClient(cfwId);
		}
		sipTransactions.erase(t->getCallId());
		sipConnections.erase(t->getConnectionId());
		// Free the masqueraded connections first
		string connectionId = t->getConnectionId();
		MediaCtrlConnection *connection = endpointConnections[connectionId];
		t->unsetSipManager(NULL);
//		t->unsetSipManager(connection);
		if(connection != NULL) {
			endpointConnections.erase(connectionId);
			delete connection;
		}
		delete t;	// FIXME
	}
//	mSip.leave();
}

void MediaCtrl::onNewSession(ClientInviteSessionHandle cis, InviteSession::OfferAnswerType, const SipMessage& msg)
{
	cout << "InviteSessionHandler::onNewSession" << endl;
}

void MediaCtrl::onFailure(ClientInviteSessionHandle cis, const SipMessage& msg)
{
	cout << "InviteSessionHandler::onFailure" << endl;
}

void MediaCtrl::onEarlyMedia(ClientInviteSessionHandle cis, const SipMessage&, const SdpContents& sdp)
{
	cout << "InviteSessionHandler::onEarlyMedia" << endl;
}

void MediaCtrl::onProvisional(ClientInviteSessionHandle cis, const SipMessage& msg)
{
	cout << "InviteSessionHandler::onProvisional" << endl;
}

void MediaCtrl::onConnected(ClientInviteSessionHandle cis, const SipMessage& msg)
{
	cout << "InviteSessionHandler::onConnected" << endl;
}

void MediaCtrl::onForkDestroyed(ClientInviteSessionHandle cis)
{
	cout << "InviteSessionHandler::onForkDestroyed" << endl;
}

void MediaCtrl::onRedirected(ClientInviteSessionHandle cis, const SipMessage& msg)
{
	cout << "InviteSessionHandler::onRedirect" << endl;
}

void MediaCtrl::onAnswer(InviteSessionHandle is, const SipMessage&, const SdpContents& sdp)
{
	cout << "InviteSessionHandler::onAnswer" << endl;
}

void MediaCtrl::onOfferRequired(InviteSessionHandle is, const SipMessage& msg)
{
	cout << "InviteSessionHandler::onOfferRequired" << endl;
}

void MediaCtrl::onOfferRejected(InviteSessionHandle is, const SipMessage* msg)
{
	cout << "InviteSessionHandler::onOfferRejected" << endl;
}

void MediaCtrl::onInfo(InviteSessionHandle is, const SipMessage& msg)
{
	cout << "InviteSessionHandler::onInfo" << endl;
}

void MediaCtrl::onInfoSuccess(InviteSessionHandle is, const SipMessage& msg)
{
	cout << "InviteSessionHandler::onInfoSuccess" << endl;
}

void MediaCtrl::onInfoFailure(InviteSessionHandle is, const SipMessage& msg)
{
	cout << "InviteSessionHandler::onInfoFailure" << endl;
}

void MediaCtrl::onMessage(InviteSessionHandle is, const SipMessage& msg)
{
	cout << "InviteSessionHandler::onMessage" << endl;
}

void MediaCtrl::onMessageSuccess(InviteSessionHandle is, const SipMessage& msg)
{
	cout << "InviteSessionHandler::onMessageSuccess" << endl;
}

void MediaCtrl::onMessageFailure(InviteSessionHandle is, const SipMessage& msg)
{
	cout << "InviteSessionHandler::onMessageFailure" << endl;
}

void MediaCtrl::onRefer(InviteSessionHandle is, ServerSubscriptionHandle, const SipMessage& msg)
{
	cout << "InviteSessionHandler::onRefer" << endl;
}

void MediaCtrl::onReferNoSub(InviteSessionHandle is, const SipMessage& msg)
{
	cout << "InviteSessionHandler::onReferNoSub" << endl;
}

void MediaCtrl::onReferRejected(InviteSessionHandle is, const SipMessage& msg)
{
	cout << "InviteSessionHandler::onReferRejected" << endl;
}

void MediaCtrl::onReferAccepted(InviteSessionHandle is, ClientSubscriptionHandle, const SipMessage& msg)
{
	cout << "InviteSessionHandler::onReferAccepted" << endl;
}

// REGISTER stuff
void MediaCtrl::onRefresh(ServerRegistrationHandle srh, const SipMessage& reg)
{
	cout << "ServerRegistrationHandler::onRefresh" << reg.brief() << endl;
	srh->accept();
}

void MediaCtrl::onRemove(ServerRegistrationHandle srh, const SipMessage& reg)
{
	cout << "ServerRegistrationHandler::onRemove" << reg.brief() << endl;
	srh->accept();
}

void MediaCtrl::onRemoveAll(ServerRegistrationHandle srh, const SipMessage& reg)
{
	cout << "ServerRegistrationHandler::onRemoveAll" << reg.brief() << endl;
	srh->accept();
}

void MediaCtrl::onAdd(ServerRegistrationHandle srh, const SipMessage& reg)
{
	cout << "ServerRegistrationHandler::onAdd" << reg.brief() << endl;
	srh->accept();
}

void MediaCtrl::onQuery(ServerRegistrationHandle srh, const SipMessage& reg)
{
	cout << "ServerRegistrationHandler::onQuery" << reg.brief() << endl;
	srh->accept();
}


// FIXME Create better logging mechanism
int MediaCtrl::remoteMonitorQuery(RemoteMonitor *monitor, RemoteMonitorRequest *request)
{
	if(!monitor || ! request)
		return -1;
	string text = request->getRequest();
	cout << "[SIP] Received remote monitor request: " << text << endl;
	if(text == "hello") {
		*request->addToResponse() << "hello, I'm " << sipName << "\r\n";
		*request->addToResponse() << "how are you?" << "\r\n";
		return 0;
	} else if(text == "help") {
		*request->addToResponse() << "Allowed requests:" << "\r\n";
		*request->addToResponse() << "\thello" << "\r\n";
		*request->addToResponse() << "\thelp" << "\r\n";
		*request->addToResponse() << "\tsip" << "\r\n";
		*request->addToResponse() << "\tcfw all|transactions|clients|<pkg name>" << "\r\n";
		return 0;
	} else if(text == "sip") {	// Some SIP-related request
		*request->addToResponse() << "SIP:" << "\r\n";
		*request->addToResponse() << "\tsip:" << sipName << "@" << sipAddressString << ":" << dec << sipPort << "\r\n";
		map<string, MediaCtrlSipTransaction *>::iterator iter;
		MediaCtrlSipTransaction *t = NULL;
		for(iter = sipTransactions.begin(); iter != sipTransactions.end(); iter++) {
			t = iter->second;
			if(t == NULL)
				continue;
			*request->addToResponse() << "\t\tTransaction: " << t->getCallId() << "\r\n";
			*request->addToResponse() << "\t\t\tApplication Server: " << t->isAS() << "\r\n";
			*request->addToResponse() << "\t\t\tConnectionId: " << t->getConnectionId() << "\r\n";
			list<string> labels = t->getMediaLabels();
			if(labels.empty())
				continue;
			list<string>::iterator label;
			for(label = labels.begin(); label != labels.end(); label++) {
				string labelName = (*label);
				*request->addToResponse() << "\t\t\tMedium: " << "\r\n";
				*request->addToResponse() << "\t\t\t\tLabel: " << labelName << "\r\n";
				*request->addToResponse() << "\t\t\t\tConnectionId: " << t->getConnectionId(labelName) << "\r\n";
				MediaCtrlRtpChannel *rtp = t->getRtpChannel(labelName);
				if(rtp == NULL)
					continue;
				*request->addToResponse() << "\t\t\t\tPort: " << rtp->getSrcPort() << "\r\n";
				*request->addToResponse() << "\t\t\t\tPeer: " << rtp->getDstIp() << ":" << rtp->getDstPort() << "\r\n";
			}
		}
		return 0;
	} else if(text.find("cfw ") == 0) {	// Some CFW-related request
		string what = text.substr(4);
		string info = cfw->getInfo(what);
		if(info == "")
			return -1;
		*request->addToResponse() << info << "\r\n";
		return 0;
	}
	return -1;
}

// Configuration parsing
void XMLCALL startElement(void *msg, const char *name, const char **atts)
{
	MediaCtrlConfValue *confValue = (MediaCtrlConfValue *)msg;
	if(confValue->scanonly)
		return;
	if(confValue->stop)
		return;
	confValue->level++;
	confValue->childs.push_back(name);

	if(confValue->childs.back() == confValue->package) {
		confValue->inPackage = true;
	} else if(confValue->childs.back() == confValue->element) {
		if(confValue->attribute == "")
			return;		// We want the element value, not one of its attributes
		else if((confValue->package != "") && !confValue->inPackage)
			return;		// The element is right, but is not a child of the package we need
		else {		// Get the right attribute
			int i=0;
			while(atts[i]) {
				if(!atts[i])
					break;
				if(!strcmp(atts[i], confValue->attribute.c_str())) {
					confValue->result = atts[i+1];
					break;
				}
				i += 2;
			}
			confValue->stop = true;
			return;
		}
	}
}

void XMLCALL valueElement(void *msg, const XML_Char *s, int len)
{
	MediaCtrlConfValue *confValue = (MediaCtrlConfValue *)msg;
	if(confValue->scanonly)
		return;
	if(confValue->stop)
		return;
	if(confValue->attribute != "")
		return;		// We want one of its attributes
	if((confValue->package != "") && !confValue->inPackage)
		return;		// The element is right, but is not a child of the package we need
	if(confValue->childs.back() == confValue->element) {
		char value[len+1];
		memcpy(value, s, len+1);
		value[len] = '\0';
		confValue->result = value;
		confValue->stop = true;
		return;
	}
}

void XMLCALL endElement(void *msg, const char *name)
{
	MediaCtrlConfValue *confValue = (MediaCtrlConfValue *)msg;
	if(confValue->scanonly)
		return;
	if(confValue->stop)
		return;
	confValue->level--;
	confValue->childs.pop_back();
	if(confValue->level == 0) {	// Finished
		if(confValue->result == "")
			cout << "[XML] No result found..." << endl;
	}
}
