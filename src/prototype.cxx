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

/*! \page DEPS Dependencies
 *
 * The application and the plugins depend on the following open source software and libraries, so make sure you install them (the development versions, not just the runtime libraries of course) before attempting a compilation:
 *
 * \li <b>GNU Common-C++:</b> http://www.gnu.org/software/commoncpp/
 * \li \b reSIProcate: http://www.resiprocate.org/
 * \li \b oRTP: http://www.linphone.org/
 * \li \b Expat: http://expat.sourceforge.net/
 * \li \b libcurl: http://curl.haxx.se/
 * \li \b Boost: http://www.boost.org/
 * \li \b OpenSSL: http://www.openssl.org/
 * \li <b>libgsm and ffmpeg:</b> http://ffmpeg.mplayerhq.hu/
 *
 * In case you install them, or have them installed, in non-standard paths, make sure to edit all the Makefiles accordingly.
 */

/*! \page CREDITS CREDITS
 *  \verbinclude AUTHORS
 */

/*! \page COPYING COPYING
 *  \verbinclude COPYING
 */

/*! \page README README
 *  \verbinclude README
 */

/*! \page CONFIG Configuration file
 *
 * This is a sample of the XML configuration file the application uses. The available configuration sections are:
 *
 * \li \b sip: for SIP-related stuff (e.g. the port to bind on);
 * \li \b cfw: for CFW-related stuff;
 * \li \b packages: for all that is related to the control packages, mainly the folder containing the plugins and optionally some package-specific settings, if needed by the package itself;
 * \li \b codecs: the folder containing the codec plugins;
 * \li \b monitor: the (proprietary) auditing port.
 *
 *  \verbinclude configuration.xml.sample
 *
 * Checkout the \ref README for more in-depth explainations about how to configure the application.
 */

/*! \defgroup core Core
 */

/*! \defgroup utils Helpers and Utilities
 */

/*!
 * \mainpage MEDIACTRL - IETF Media Server Control Prototype
 *
 * \par Developer Documentation for the MEDIACTRL Prototype
 * This is the main developer documentation for the MEDIACTRL
 * prototype, generated with the help of Doxygen. Make sure you
 * check the \ref DEPS before attempting a compilation. If you are
 * interested in how to use, and maybe extend, the prototype,
 * checkout the \ref README and the \ref CONFIG as well.
 *
 * \section changes Changes from version 0.3.0 to 0.4.0
 * \li fixed several bugs (e.g. error notification and mapping with clients, etc.)
 * \li updates to latest drafts:
 * <ul>
 * <li> Framework (draft-ietf-mediactrl-sip-control-framework-10)
 * <ul>
 * <li> bugfixes
 * <li> 202 + mandatory timeout
 * <li> better handling of the 403 error (according to security considerations)
 * <li> moved all the extended transaction logic in CfwStack (packages don't know anything about it)
 * <li> added support for TCP/TLS/CFW
 * </ul>
 * <li> Mixer package (draft-ietf-mediactrl-mixer-control-package-06)
 * <ul>
 * <li> bugfixes and changes according to the new spec
 * <li> new error codes
 * <li> audio mixing rewritten from scratch (should be better now)
 * <li> implemented <volume>
 * <li> implemented new direction-based <stream> mechanism
 * <li> enforced security considerations
 * </ul>
 * <li> IVR package (draft-ietf-mediactrl-ivr-control-package-06)
 * <ul>
 * <li> bugfixes and changes according to the new spec
 * <li> new error codes
 * <li> renamed 'src' to 'loc'
 * <li> updated <prompt> and <record> mechanisms with new <media>
 * <li> implemented parallel recording and upload to specific locations (use of <media> and libcurl)
 * <li> implemented new parallel mechanism for playback (<par>/<seq>, multiple audio tracks allowed as a whole for each dialog, A*N/V, default N=4)
 * <li> fixed sendonly/recvonly
 * <li> updated effect of VCR controls (e.g. volume resumes from paused, etc)
 * <li> enforced security considerations
 * <li> started work on SRGS support
 * </ul>
 * <li> Removed expired VoiceXML IVR Package
 * </ul>
 * 
 * \section changes Changes from version 0.2 to 0.3
 *
 * \li updates according to the new drafts:
 * <ul>
 * <li> core SCFW --> CFW
 * <li> SYNCH --> SYNC and cfw-id attribute
 * <li> updated IVR package (draft-ietf-mediactrl-ivr-control-package-02)
 * <ul>
 * <li> msc-ivr-basic --> msc-ivr
 * <li> "Media Server Control - Interactive Voice Response - Basic - version 1.0" --> "Media Server Control - Interactive Voice Response - version 1.0"
 * <li> added 'control' for VCR, and added new keys (start, end, speed)
 * <li> updated existing models (prompt, collect, record)
 * </ul>
 * <li> updated Mixer package (draft-ietf-mediactrl-mixer-control-package-02)
 * <ul>
 * <li> msc-conf-audio --> msc-mixer
 * <li> "Media Server Control - Conferencing - Audio - version 1.0" --> "Media Server Control - Mixer - version 1.0"
 * <li> bla bla
 * </ul>
 * <li> etc.etc...
 * </ul>
 * \li renamed BasicIvr* classes to Ivr*
 * \li renamed AudioConf* classes to Mixer*
 * \li added support for <stream> to the IVR package
 * \li added basic VAD to the recording context (vadinitial+timeout=noinput)
 * \li added support for H.264 (proven compatible with both Grandstream GXV3000 and Ekiga 3.0)
 *
 * \section changes Changes from version 0.1 to 0.2
 *
 * \li added configure script for easier installation
 * \li updates according to the new drafts:
 * <ul>
 * <li> core ESCS --> SCFW (new 'Content-Type' header; use of CONTROL for events; etc.)
 * <li> SYNCH --> SYNC
 * <li> added support for new cfw-id SDP attribute
 * <li> new IVR package (use of CONTROL for events; dialog state machine; new models; VCR controls; variable announcements; etc.)
 * <li> removed 'pending' state and implemented use of 'update'
 * <li> etc.etc...
 * </ul>
 * \li added support for draft-boulton-mmusic-sdp-control-package-attribute-02
 * \li boost::regex library for parsing and validation
 * \li no more ffmpeg for mu-Law and a-Law codecs (now based on http://hazelware.luggle.com/tutorials/mulawcompression.html)
 * \li added explicit clockrate for codecs (still to be fixed)
 * \li removed StreamLogger class, logging must be accomplished externally (e.g. './mediactrl | tee mylogfile.log')
 *
 * \section copyright Copyright and author
 *
 * Copyright (C) 2007 - 2009, COMICS Research Group -- Meetecho
 * http://www.comics.unina.it/ -- http://www.meetecho.com/
 *
 * \author Lorenzo Miniero <lorenzo.miniero@unina.it> ( \ref CREDITS )
 *
 * \section license License
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the COPYING file
 * at the top of the source tree. For more details see \ref COPYING
 *
*/


/*! \file
 *
 * \brief MediaCtrl Main Application
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <iostream>
#include <cstdlib>

#include "MediaCtrlMemory.h"
#include "MediaCtrl.h"

using namespace std;
using namespace mediactrl;

static MediaCtrl *mc;
void print_help(string exe);
void handle_signal(int signum);
void handle_signal_inahurry(int signum);
void handle_signal_garbage(int signum);
pthread_cond_t shutDown;
pthread_mutex_t mShutDown;

/*!
 * \brief main
 * This is called when the application starts: the core MediaCtrl is instanciated here
 */
int main(int argc, char *argv[])
{
	// Setup the memory handler
	MCMINIT();
	char *test1 = (char *)MCMALLOC(20, sizeof(char));
	if(!test1) {
		cout << "\tAllocation failed! Leaving..." << endl;
		exit(-1);
	}
	char *test2 = (char *)MCMREALLOC(test1, 50);
	if(!test2) {
		cout << "\tRe-allocation failed! Leaving..." << endl;
		exit(-1);
	}
	test1 = test2;
	MCMFREE(test1);
	test1 = NULL;
	test2 = NULL;
	cout << "\tAll tests passed" << endl;
	// Start the Media Server
	cout << endl;
	cout << "-------------------------" << endl;
	cout << "MediaCtrl prototype 0.4.0" << endl;
	cout << "-------------------------" << endl << endl;
	string conf = "";
	if(argc == 1) {
		conf = DEFAULT_CONF_FILE;
		cout << "Using default configuration file: " << endl;
		cout << "\t" << conf << endl;
	} else {
		string arg = argv[1];
		if((arg == "-h") || (arg == "--help")) {
			print_help(argv[0]);
			exit(0);
		} else if((arg == "-c") || (arg == "--conf")) {
			if(argc == 1) {
				cout << "Missing mandatory path to configuration file" << endl;
				print_help(argv[0]);
				exit(-1);
			} else {
				conf = argv[2];
				cout << "Using configuration file: " << endl;
				cout << "\t" << conf << endl;
			}
		} else {
			cout << "Unrecognized option '" << argv[1] << "'" << endl;
			print_help(argv[0]);
			exit(-1);
		}
	}

	time_t curtime;
	time(&curtime);
	char *timeStr = ctime((const time_t *)&curtime);
	string tmp = timeStr;
	tmp = tmp.substr(0, tmp.length()-1);
	cout << endl << endl << "[" + tmp + "]" << endl << endl << endl;
	int i = 0;
	cout << "Executed:";
	while(i < argc) {
		cout << " " << argv[i];
		i++;
	}
	cout << endl << endl;

	pthread_cond_init(&shutDown, NULL);
	pthread_mutex_init(&mShutDown, NULL);
	signal(SIGINT, handle_signal);

	cout << "Creating MediaCtrl object..." << endl;

	startCollector();
	mc = new MediaCtrl(conf);
	if(!mc) {
		cout << "FAILED" << endl;
		return -1;
	}
	cout << "SUCCESS" << endl;
	mc->run();

	pthread_mutex_lock(&mShutDown);
	pthread_cond_wait(&shutDown, &mShutDown);
	pthread_mutex_unlock(&mShutDown);

	cout << "\tDestroying MediaCtrl..." << endl;
	if(mc)
		delete mc;
	stopCollector();

	cout << "Bye." << endl;

	return EXIT_SUCCESS;
}

/*!
 * \brief Helper method to show up the instructions
 * \param exe The executable as it has been launched
*/
void print_help(string exe)
{
	cout << "Usage: " << exe << "\twithout arguments: use configuration.xml from this folder" << endl;
	cout << "\t\t\t-h|--help\t\t(Print this help)" << endl;
	cout << "\t\t\t-c|--conf conffile.xml\t(Use conffile.xml as configuration)" << endl;
}

/*!
 * \brief Signal interceptor (CTRL+C) the first time it is called
 * \param signum The intercepted signal
 */
void handle_signal(int signum)
{
	signal(SIGINT, handle_signal_inahurry);
	cout << "Received SIGINT, preparing cleanup..." << endl;
	pthread_cond_signal(&shutDown);
}

/*!
 * \brief Signal interceptor (CTRL+C) the second time it is called
 * \note When CTRL+C is pressed a second time, we know the user is in a hurry...
 * \param signum The intercepted signal
 */
void handle_signal_inahurry(int signum)
{
	signal(SIGINT, handle_signal_garbage);
	cout << "*Already* received SIGINT, can't you really wait for cleanup??" << endl;
}

/*!
 * \brief Signal interceptor (CTRL+C) the third time it is called
 * \note When CTRL+C is pressed a third time before quitting, the freeing process stops and all the garbage is left around
 * \param signum The intercepted signal
 */
void handle_signal_garbage(int signum)
{
	signal(SIGINT, NULL);
	cout << "Ok ok, I'll just leave the garbage around..." << endl;
	kill(getpid(), SIGINT);
}
