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
 * \brief IVR Control Package (draft-ietf-mediactrl-ivr-control-package-06)
 *
 * \author Lorenzo Miniero <lorenzo.miniero@unina.it>
 *
 * \ingroup packages
 * \ref packages
 */

#include "expat.h"
#include "curl/curl.h"
#include <boost/regex.hpp>
#include "ControlPackage.h"

#include <dirent.h>
#include <stdio.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <math.h>
#include <limits.h>


extern "C" {
#ifdef FFMPEG_ALTDIR
#include <libavformat/avformat.h>	// FFmpeg libavformat
#include <libavcodec/avcodec.h>	// FFmpeg libavcodec
#include <libavutil/fifo.h>	// FFmpeg fifo handler (for audio resampling)
#else
#include <ffmpeg/avformat.h>	// FFmpeg libavformat
#include <ffmpeg/avcodec.h>	// FFmpeg libavcodec
#include <ffmpeg/fifo.h>	// FFmpeg fifo handler (for audio resampling)
#endif
}
static bool ffmpeg_initialized = false;


using namespace mediactrl;
using namespace boost;

static string webServerPath;


/**
* \brief Small utility to generate random strings
* \note This small helper method is used to generate random identifiers
*/
string random_string(size_t size);
string random_string(size_t size)
{
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

/**
* \brief Small utility to generate timestamp (needed for DTMF notification)
* \note This small helper method is used to generate DTMF timestamps
*/
string dtmfTimestamp();
string dtmfTimestamp()
{
	// e.g. timestamp="2008-05-12T12:13:14Z"
	struct tm *timeinfo;
	time_t rawtime;
	char buffer[80];
	time(&rawtime);
	timeinfo = localtime(&rawtime);
	strftime(buffer, 80, "%Y-%m-%dT%H:%M:%SZ", timeinfo);	// FIXME
	return buffer;
}

/**
* \brief Small utility to check if an audio frame contains just silence or not (crappy and quick)
* \note This method is supposed to return true if an audio frame only contains silence: it just checks if all the samples are (in absolute) below 3000 (~10% of 32k), so it's a very dirty hack, a placeholder for future better VAD implementations
*/
bool isSilence(MediaCtrlFrame *frame);
bool isSilence(MediaCtrlFrame *frame)
{
	if((frame == NULL) || (frame->getMediaType() != MEDIACTRL_MEDIA_AUDIO) || (frame->getBuffer() == NULL) || (frame->getFormat() != MEDIACTRL_RAW))
		return false;
	bool silence = true;
	int i=0;
	short int *buffer = (short int *)frame->getBuffer();
	short int sample = 0;
	for(i=0; i < frame->getLen(); i+=2) {
		sample = abs(*buffer);
		buffer++;
		if(sample > 3000) {
			silence = false;	// At least a sample is above our threshold, so it's not silence (FIXME)
			break;
		}
	}
	return silence;
}


/// Helper struct to generate and parse WAVE headers
typedef struct wavHdr {
	char riff[4];
	uint32_t len;
	char wave[4];
	char fmt[4];
	uint32_t formatsize;
	uint16_t format;
	uint16_t channels;
	uint32_t samplerate;
	uint32_t avgbyterate;
	uint16_t samplebytes;
	uint16_t channelbits;
	char data[4];
	uint32_t blocksize;
} wavHdr;

/// Dialog states
enum dialog_states {
	/*! Currently inactive dialog */
	DIALOG_IDLE = 0,
	/*! Dialog is being prepared */
	DIALOG_PREPARING,
	/*! Dialog has been prepared */
	DIALOG_PREPARED,
	/*! Dialog is being started */
	DIALOG_STARTING,
	/*! Dialog has been started */
	DIALOG_STARTED,
	/*! Dialog has terminated */
	DIALOG_TERMINATED,
};


/// Available models
/*! \<prompt\> */
#define MODEL_PROMPT 		(1 << 0)
/*! \<collect\> */
#define MODEL_COLLECT 		(1 << 1)
/*! \<control\> */
#define MODEL_CONTROL 		(1 << 2)
/*! \<record\> */
#define MODEL_RECORD 		(1 << 3)


/// dialogexit::status
enum {
	/*! dialogterminate */
	DIALOGEXIT_DIALOGTERMINATE = 0,
	/*! success */
	DIALOGEXIT_SUCCESS,
	/*! connection/conference termination */
	DIALOGEXIT_CONTERMINATE,
	/*! maximum duration */
	DIALOGEXIT_MAXDURATION,
	/*! execution error */
	DIALOGEXIT_ERROR,
};


/// Allowed termmodes
enum {
	/*! Unknown reason */
	TERMMODE_UNKNOWN = -1,
	/*! prompt::completed */
	TERMMODE_COMPLETED,
	/*! prompt::maxduration */
	TERMMODE_MAXDURATION,
	/*! prompt::bargein */
	TERMMODE_BARGEIN,
	/*! prompt/collect/record::stopped */
	TERMMODE_STOPPED,
	/*! collect::match */
	TERMMODE_MATCH,
	/*! collect/record::noinput */
	TERMMODE_NOINPUT,
	/*! collect::nomatch */
	TERMMODE_NOMATCH,
	/*! record::dtmf */
	TERMMODE_DTMF,
	/*! record::maxtime */
	TERMMODE_MAXTIME,
	/*! record::finalsilence */
	TERMMODE_FINALSILENCE,
};


/// Allowed VCR controls
enum {
	/*! Unknown VCR */
	VCR_NONE = -1,
	/*! Goto Start */
	VCR_START,
	/*! Goto End */
	VCR_END,
	/*! Fast Forward */
	VCR_FF,
	/*! Rewind */
	VCR_RW,
	/*! Pause */
	VCR_PAUSE,
	/*! Resume */
	VCR_RESUME,
	/*! Volume Up */
	VCR_VOLUP,
	/*! Volume Down */
	VCR_VOLDOWN,
	/*! Speed Up */
	VCR_SPEEDUP,
	/*! Speed Down */
	VCR_SPEEDDOWN,
};


/// TimeLine related ways of adding a new <prompt> element
enum timeline_ways {
	/*! Normal (no sequence, no parallel; proceed to next step) */
	TIMELINE_NORMAL = 0,
	/*! Parallel (add prompt in parallel to the one that already exists) */
	TIMELINE_PAR,
	/*! Sequential (add prompt to the ones of the same type that already exist) */
	TIMELINE_SEQ,
};


/// EndSync for parallel playback
enum endsync_ways {
	/*! No syncing (not a parallel TimeLine) */
	ENDSYNC_NONE = -1,
	/*! First (when first ends, step ends) */
	ENDSYNC_FIRST,
	/*! Last (when last ends, step ends) */
	ENDSYNC_LAST,
};


/// Prompt status (e.g. still downloading, ready, etc.)
enum prompt_status {
	/*! Invalid prompt */
	PROMPT_INVALID = -1,
	/*! No status yet */
	PROMPT_NONE,
	/*! Downloading */
	PROMPT_DOWNLOADING,
	/*! Retrieved/ready */
	PROMPT_RETRIEVED,
};


/// Connection types
enum connection_types {
	/*! Connection */
	CONNECTION = 0,
	/*! Conference */
	CONFERENCE,
};


/// Number of allowed parallel audio tracks
#define TRACKS	4


// Boost parsers for CONTROL bodies
/// IVR MIME Type Boost-Regex parser
int checkType(string type)
{
	regex re;
	re.assign("application/msc-ivr+xml", regex_constants::icase);
	if(!regex_match(type.c_str(), re))
		return -1;
	return 0;
}

/// Directionality Boost-Regex parser
int parseDirection(string direction)
{
	regex re;
	re.assign("sendrecv", regex_constants::icase);
	if(regex_match(direction.c_str(), re))
		return SENDRECV;
	re.assign("sendonly", regex_constants::icase);
	if(regex_match(direction.c_str(), re))
		return SENDONLY;
	re.assign("recvonly", regex_constants::icase);
	if(regex_match(direction.c_str(), re))
		return RECVONLY;
	re.assign("inactive", regex_constants::icase);
	if(regex_match(direction.c_str(), re))
		return INACTIVE;
	return -1;
}

/// Boolean Boost-Regex parser
bool booleanValue(string value, bool *ok)
{
	*ok = true;
	bool result = false;
	regex re;
	re.assign("true|yes|1", regex_constants::icase);	// FIXME Should be true only?
	if(!regex_match(value.c_str(), re)) {
		re.assign("false|no|0", regex_constants::icase);	// FIXME Should be false only?
		if(!regex_match(value.c_str(), re))
			*ok = false;
		else
			result = false;
	} else
		result = true;

	return result;
}

/// Positive Integer Boost-Regex parser
uint16_t positiveInteger(string value, bool *ok)
{
	*ok = true;
	uint16_t result = 0;
	regex re;
	cmatch matches;
	re.assign("\\+?(\\d+)", regex_constants::icase);
	if(!regex_match(value.c_str(), matches, re))
		*ok = false;
	else {
		string match(matches[1].first, matches[1].second);
		result = atoi(match.c_str());
	}

	return result;
}

/// TimeDesignation Boost-Regex parser
uint32_t timeDesignation(string value, bool *ok)
{
	*ok = true;
	uint32_t time = 0;
	regex re;
	cmatch matches;
	re.assign("\\+?(\\d+)?\\.*(\\d+)?(s|ms)", regex_constants::icase);
	if(!regex_match(value.c_str(), matches, re))
		*ok = false;
	else {
		int m = 0;
		// 3. ms or s?
		string match(matches[3].first, matches[3].second);
		if(match == "s")
			m = 1000;	// s
		else
			m = 1;		// ms
		// 1. integer part
		if(matches[1].first != matches[1].second) {
			string match(matches[1].first, matches[1].second);
			time += atoi(match.c_str())*m;
		}
		// 2. decimal part
		if(matches[2].first != matches[2].second) {
			string match(matches[2].first, matches[2].second);
			time += atoi(match.c_str())*(m/10);
		}
	}
	return time;
}

/// Percentage Boost-Regex parser
uint16_t percentValue(string value, bool *ok)
{
	*ok = true;
	uint16_t result = 0;
	regex re;
	cmatch matches;
	re.assign("\\+?(\\d|\\d{2}|\\d{3})%", regex_constants::icase);
	if(!regex_match(value.c_str(), matches, re))
		*ok = false;
	else {
		string match(matches[1].first, matches[1].second);
		result = atoi(match.c_str());
	}

	return result;
}

/// Digit Boost-Regex parser
int dtmfDigit(string value, bool *ok)
{
	*ok = true;
	int result = MEDIACTRL_DTMF_NONE;
	if(value.length() != 1)
		*ok = false;
	else {
		if(value == "0")
			result = MEDIACTRL_DTMF_0;
		else if(value == "1")
			result = MEDIACTRL_DTMF_1;
		else if(value == "2")
			result = MEDIACTRL_DTMF_2;
		else if(value == "3")
			result = MEDIACTRL_DTMF_3;
		else if(value == "4")
			result = MEDIACTRL_DTMF_4;
		else if(value == "5")
			result = MEDIACTRL_DTMF_5;
		else if(value == "6")
			result = MEDIACTRL_DTMF_6;
		else if(value == "7")
			result = MEDIACTRL_DTMF_7;
		else if(value == "8")
			result = MEDIACTRL_DTMF_8;
		else if(value == "9")
			result = MEDIACTRL_DTMF_9;
		else if(value == "*")
			result = MEDIACTRL_DTMF_STAR;
		else if(value == "#")
			result = MEDIACTRL_DTMF_POUND;
		else if(value == "A")
			result = MEDIACTRL_DTMF_A;
		else if(value == "B")
			result = MEDIACTRL_DTMF_B;
		else if(value == "C")
			result = MEDIACTRL_DTMF_C;
		else if(value == "D")
			result = MEDIACTRL_DTMF_D;
		else
			*ok = false;
	}
	return result;
}

/// Digit Boost-Regex parser
string dtmfCode(int digit)
{
	if((digit < MEDIACTRL_DTMF_0) || (digit > MEDIACTRL_DTMF_D))
		return "";
	switch(digit) {
		case MEDIACTRL_DTMF_0:
			return "0";
		case MEDIACTRL_DTMF_1:
			return "1";
		case MEDIACTRL_DTMF_2:
			return "2";
		case MEDIACTRL_DTMF_3:
			return "3";
		case MEDIACTRL_DTMF_4:
			return "4";
		case MEDIACTRL_DTMF_5:
			return "5";
		case MEDIACTRL_DTMF_6:
			return "6";
		case MEDIACTRL_DTMF_7:
			return "7";
		case MEDIACTRL_DTMF_8:
			return "8";
		case MEDIACTRL_DTMF_9:
			return "9";
		case MEDIACTRL_DTMF_STAR:
			return "*";
		case MEDIACTRL_DTMF_POUND:
			return "#";
		case MEDIACTRL_DTMF_A:
			return "A";
		case MEDIACTRL_DTMF_B:
			return "B";
		case MEDIACTRL_DTMF_C:
			return "C";
		case MEDIACTRL_DTMF_D:
			return "D";
		default:
			return "";
	}
	return "";
}


// cURL helper for external references
size_t write_dataER(void *buffer, size_t size, size_t nmemb, void *userp)
{
	stringstream *content = (stringstream*)userp;
	(*content) << (char *)(buffer);
	return size*nmemb;
}

/// cURL retriever for external references
string getExternalReference(string url, uint32_t fetchtimeout, long int *code)
{
	// FIXME Fix fetchtimeout
	cout << "[IVR] Creating CURL handler for new external reference: " << url << endl;
	CURL *handle = curl_easy_init();
	if(handle == NULL) {
		*code = -1;
		return "";
	}
	stringstream buffer;
	curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
	curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, write_dataER);
	curl_easy_setopt(handle, CURLOPT_WRITEDATA, &buffer);
	curl_easy_setopt(handle, CURLOPT_VERBOSE, 1);
	int err = curl_easy_perform(handle);
	if(err < 0) {
		curl_easy_cleanup(handle);
		*code = -1;
		return "";
	}
	curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, code);
	curl_easy_cleanup(handle);
	return buffer.str();
}


/// cURL uploader
int uploadFile(string url, string filename)
{
	cout << "[IVR] Creating CURL handler for new upload: " << url << endl;
	CURL *handle = curl_easy_init();
	if(handle == NULL)
		return -1;
	FILE *file = fopen(filename.c_str(), "rb");
	if(file == NULL)
		return -1;
	struct curl_slist *slist=NULL;
	slist = curl_slist_append(slist, "Expect:");	// FIXME
	curl_easy_setopt(handle, CURLOPT_HTTPHEADER, slist);
	curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
	curl_easy_setopt(handle, CURLOPT_UPLOAD, 1);
	curl_easy_setopt(handle, CURLOPT_READDATA, file);
	curl_easy_setopt(handle, CURLOPT_VERBOSE, 1);
	int err = curl_easy_perform(handle);
	long int code = 0;
	curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &code);
	curl_slist_free_all(slist);
	curl_easy_cleanup(handle);
	if(err < 0)
		return -1;
	if(code >= 400)	// FIXME
		return code;
	return 0;
}


/// Filename builder for 'variable' announcements
list<string> *buildVariableFilenames(string value, string type, string format, string lang="")
{
	if((value == "") || (type == ""))
		return NULL;
	// TODO We ignore language and gender currently
	string base = "file://" + webServerPath + "/prompts/digits/";	// FIXME
	regex re;
	cmatch matches;
	re.assign("date", regex_constants::icase);
	if(regex_match(type.c_str(), re)) {
		if(format == "")
			format = "ymd";	// Default format for the 'date' type
		re.assign("mdy|ymd|dmy|dm", regex_constants::icase);
		if(!regex_match(format.c_str(), matches, re)) {
			cout << "[IVR] Couldn't evaluate format " << format << endl;
			return NULL;
		}
		uint16_t y = 0, m = 0, d = 0;
		// First of all extract the date information
		re.assign("(\\d{2}|\\d{4})\\-(\\d{1,2})\\-(\\d{1,2})", regex_constants::icase);
		if(!regex_match(value.c_str(), matches, re)) {
			cout << "[IVR] Couldn't match format " << format << " for type " << type << " (value " << value << ")" << endl;
			return NULL;
		}
		string year = string(matches[1].first, matches[1].second);
		if(atoi(year.c_str()) < 100)
			year = "20" + string(matches[1].first, matches[1].second);
		string month = string(matches[2].first, matches[2].second);
		string day = string(matches[3].first, matches[3].second);
		y = atoi(year.c_str());
		m = atoi(month.c_str());
		d = atoi(day.c_str());
		// Now workout the filenames		
		cout << "[IVR] Year=" << dec << y << " Month=" << dec << m << " Day=" << dec << d << endl;
		if(d > 31) {
			cout << "[IVR] \tDay > 31?" << endl;
			return NULL;
		}
		if((m == 0) || (m > 12)) {
			cout << "[IVR] \tThis month does not exist!" << endl;
			return NULL;
		}
		list<string> *filenames = new list<string>;
		list<string> filenamesY;
		list<string> filenamesM;
		list<string> filenamesD;
		// Day
		if(d >= 30) {
			string filename = base + "h-30.wav";
			filenamesD.push_back(filename);
		} else if(d >= 20) {
			string filename = base + "h-20.wav";
			filenamesD.push_back(filename);
		} else {
			stringstream filename;
			filename << base << "h-" << dec << d << ".wav";
			filenamesD.push_back(filename.str());
		}
		if((d > 20) && ((d%10) > 0)) {
			stringstream filename;
			filename << base << "h-" << dec << (d%10) << ".wav";
			filenamesD.push_back(filename.str());
		}
		// Month
		stringstream mFilename;
		mFilename << base << "mon-" << dec << (m-1) << ".wav";
		filenamesM.push_back(mFilename.str());
		if(y > 0) {
			// Year
			uint16_t th = y/1000, hu = (y-th*1000)/100, de = (y-th*1000-hu*100)/10, un = y-th*1000-hu*100-de*10;
			if(th) {
				stringstream filename;
				filename << base << dec << th << ".wav";
				filenamesY.push_back(filename.str());
				filenamesY.push_back(base + "thousand.wav");
			}
			if(hu) {
				stringstream filename;
				filename << base << dec << hu << ".wav";
				filenamesY.push_back(filename.str());
				filenamesY.push_back(base + "hundred.wav");
			}
			if(de > 1) {
				stringstream filename;
				filename << base << dec << de*10 << ".wav";
				filenamesY.push_back(filename.str());
			}
			if(un) {
				if(de == 1)
					un += 10;
				stringstream filename;
				filename << base << dec << un << ".wav";
				filenamesY.push_back(filename.str());
			}
		}
		string filename;
		// Now prepare them in the right order
		if(format == "mdy") {
			// Month
			if(!filenamesM.empty()) {
				while(!filenamesM.empty()) {
					filename = filenamesM.front();
					filenamesM.pop_front();
					filenames->push_back(filename);
				}
			}
			// Day
			if(!filenamesD.empty()) {
				while(!filenamesD.empty()) {
					filename = filenamesD.front();
					filenamesD.pop_front();
					filenames->push_back(filename);
				}
			}
			// Year
			if(!filenamesY.empty()) {
				while(!filenamesY.empty()) {
					filename = filenamesY.front();
					filenamesY.pop_front();
					filenames->push_back(filename);
				}
			}
		} else if(format == "ymd") {
			// Year
			if(!filenamesY.empty()) {
				while(!filenamesY.empty()) {
					filename = filenamesY.front();
					filenamesY.pop_front();
					filenames->push_back(filename);
				}
			}
			// Month
			if(!filenamesM.empty()) {
				while(!filenamesM.empty()) {
					filename = filenamesM.front();
					filenamesM.pop_front();
					filenames->push_back(filename);
				}
			}
			// Day
			if(!filenamesD.empty()) {
				while(!filenamesD.empty()) {
					filename = filenamesD.front();
					filenamesD.pop_front();
					filenames->push_back(filename);
				}
			}
		} else if(format == "dmy") {
			// Day
			if(!filenamesD.empty()) {
				while(!filenamesD.empty()) {
					filename = filenamesD.front();
					filenamesD.pop_front();
					filenames->push_back(filename);
				}
			}
			// Month
			if(!filenamesM.empty()) {
				while(!filenamesM.empty()) {
					filename = filenamesM.front();
					filenamesM.pop_front();
					filenames->push_back(filename);
				}
			}
			// Year
			if(!filenamesY.empty()) {
				while(!filenamesY.empty()) {
					filename = filenamesY.front();
					filenamesY.pop_front();
					filenames->push_back(filename);
				}
			}
		} else if(format == "dm") {
			// Day
			if(!filenamesD.empty()) {
				while(!filenamesD.empty()) {
					filename = filenamesD.front();
					filenamesD.pop_front();
					filenames->push_back(filename);
				}
			}
			// Month
			if(!filenamesM.empty()) {
				while(!filenamesM.empty()) {
					filename = filenamesM.front();
					filenamesM.pop_front();
					filenames->push_back(filename);
				}
			}
		}
		return filenames;
	}
	re.assign("time", regex_constants::icase);
	if(regex_match(type.c_str(), re)) {
		if(format == "")
			format = "t24";	// Default format for the 'time' type
		if((format != "t12") && (format != "t24")) {		// HH:MM is the only supported
			cout << "[IVR] Unsupported format " << format << " for type " << type << " (value " << value << ")" << endl;
			return NULL;
		}
		re.assign("(\\d{1,2})\\:(\\d{1,2})", regex_constants::icase);
		if(!regex_match(value.c_str(), matches, re)) {
			cout << "[IVR] Couldn't match format " << format << " for type " << type << " (value " << value << ")" << endl;
			return NULL;
		}
		string hour = string(matches[1].first, matches[1].second);
		string minutes = string(matches[2].first, matches[2].second);
		uint16_t h = atoi(hour.c_str());
		uint16_t m = atoi(minutes.c_str());
		cout << "[IVR] Hour=" << dec << h << " Minutes=" << dec << m << endl;
		bool am = true;
		if(format == "t12") {
			if(h > 12) {
				h -= 12;
				am = false;	// PM
			}
		}
		if(h > 23) {
			cout << "[IVR] \tHour > 23?" << endl;
			return NULL;
		}
		if(m > 59) {
			cout << "[IVR] \tMinutes > 59?" << endl;
			return NULL;
		}
		list<string> *filenames = new list<string>;
		// Hour
		if(h >= 20) {
			string filename = base + "20.wav";
			filenames->push_back(filename);
		} else {
			stringstream filename;
			filename << base << dec << h << ".wav";
			filenames->push_back(filename.str());
		}
		if((h > 20) && ((h%10) > 0)) {
			stringstream filename;
			filename << base << dec << (h%10) << ".wav";
			filenames->push_back(filename.str());
		}
		// Minutes
		uint16_t de = m/10, un = m-de*10;
		if(de > 1) {
			stringstream filename;
			filename << base << dec << de*10 << ".wav";
			filenames->push_back(filename.str());
		}
		if(un) {
			if(de == 1)
				un += 10;
			stringstream filename;
			filename << base << dec << un << ".wav";
			filenames->push_back(filename.str());
		}
		if(format == "t12") {
			string filename = base + (am ? "a-m" : "p-m") + ".wav";
			filenames->push_back(filename);
		}
		string filename = base + "oclock.wav";
		filenames->push_back(filename);
		return filenames;
	}
	re.assign("digits", regex_constants::icase);
	if(regex_match(type.c_str(), re)) {
		if(format == "")
			format = "gen";	// Default format for the 'digits' type
		if((format != "gen") && (format != "crn") && (format != "ord")) {		// Unsupported format
			cout << "[IVR] Unsupported format " << format << " for type " << type << " (value " << value << ")" << endl;
			return NULL;
		}
		re.assign("(\\d+)", regex_constants::icase);
		if(!regex_match(value.c_str(), matches, re)) {
			cout << "[IVR] Couldn't match type " << type << " (value " << value << ")" << endl;
			return NULL;
		}
		string digit = string(matches[1].first, matches[1].second);
		uint16_t d = atoi(digit.c_str());
		if(d > 9999)	// FIXME badly
			return NULL;
		list<string> *filenames = new list<string>;
		uint16_t th = d/1000, hu = (d-th*1000)/100, de = (d-th*1000-hu*100)/10, un = d-th*1000-hu*100-de*10;
		// TODO We currently handle only gen (and crn? or are they different? no support for ord anyway)
		if(th) {
			stringstream filename;
			filename << base << dec << th << ".wav";
			filenames->push_back(filename.str());
			filenames->push_back(base + "thousand.wav");
		}
		if(hu) {
			stringstream filename;
			filename << base << dec << hu << ".wav";
			filenames->push_back(filename.str());
			filenames->push_back(base + "hundred.wav");
		}
		if(de > 1) {
			stringstream filename;
			filename << base << dec << de*10 << ".wav";
			filenames->push_back(filename.str());
		}
		if(un) {
			if(de == 1)
				un += 10;
			stringstream filename;
			filename << base << dec << un << ".wav";
			filenames->push_back(filename.str());
		}
		return filenames;
	}
	return NULL;
}

// eXpat parser callbacks for parsing msc-ivr XML blobs
static void XMLCALL startElement(void *msg, const char *name, const char **atts);
static void XMLCALL valueElement(void *msg, const XML_Char *s, int len);
static void XMLCALL endElement(void *msg, const char *name);
// eXpat parser callbacks for parsing <dialog> sub-elements, since they might be referred externally
static void XMLCALL startElementBI(void *msg, const char *name, const char **atts);
static void XMLCALL valueElementBI(void *msg, const XML_Char *s, int len);
static void XMLCALL endElementBI(void *msg, const char *name);
// eXpat parser callbacks for parsing <grammar> sub-elements, since they might be referred externally
static void XMLCALL startElementGR(void *msg, const char *name, const char **atts);
static void XMLCALL valueElementGR(void *msg, const XML_Char *s, int len);
static void XMLCALL endElementGR(void *msg, const char *name);


class IvrPackage;
class IvrDialog;


/// Helper class to handle timelines of prompt execution (e.g. parallel/sequential prompts)
class TimeLine : public gc {
	public:
		TimeLine()
			{
				pFilenames.clear();
				pTimeouts.clear();
				pSoundlevel.clear();
				pClipbegin.clear();
				pClipend.clear();
			};
		~TimeLine() {};

		list<string> pFilenames;	// Each row contains the filename and the fetch timeout
		list<uint32_t> pTimeouts;
		list<uint32_t> pSoundlevel;
		list<uint32_t> pClipbegin;
		list<uint32_t> pClipend;
};


/// Prompt handler
class Prompt : public gc {
	public:
		Prompt(string name, string tmpFolder="");
		~Prompt();

		int startTransfer(uint32_t timeout);
		size_t writeData(void *buffer, size_t size, size_t nmemb);
		string getName() { return name; };
		string getFilename() { return filename; };
		int getStatus() { return status; };
		int wait();

	private:
		string name;		// The path in the request
		CURL *handle;		// Curl handler
		string filename;	// Path where the file has been saved
		FILE *file;			// Temporary FILE instance to save to
		int status;			// Status of the prompt (downloading? ready? etc)

		uint32_t timeout;	// The fetchtimeout for the transfer (default=30s)
		TimerPort *timer;	// Timer to check the timeout
		uint32_t startTime;	// The time the transfer has started
		bool timedOut;		// Whether the download failed because the timeout was exceeded
		ost::Conditional cond;	// Conditional used by wait()
};

class PromptInstance : public gc {
	public:
		PromptInstance(Prompt *prompt)
			{
				this->prompt = prompt;
				soundLevel = 100;
				clipBegin = 0;
				clipEnd = 0;
				file = NULL;
			};
		~PromptInstance()
			{
				if(prompt != NULL)
					cout << "[IVR] Removing PromptInstance of '" << prompt->getFilename() << "'..." << endl;
				else
					cout << "[IVR] Removing unkwnown PromptInstance..." << endl;
				closeFile();
			};

		FILE *openFile();
		void closeFile();

		void setSoundLevel(uint16_t soundLevel) { this->soundLevel = soundLevel; };
		void setClipBounds(uint32_t clipBegin, uint32_t clipEnd)
			{
				this->clipBegin = clipBegin;
				this->clipEnd = clipEnd;
			};
		Prompt *getPrompt() { return prompt; };
		uint16_t getSoundLevel() { return soundLevel; };
		uint32_t getClipBegin() { return clipBegin; };
		uint32_t getClipEnd() { return clipEnd; };

	private:
		Prompt *prompt;			// The prompt associated with this instance
		FILE *file;				// The file itself

		uint16_t soundLevel;	// Volume to play the prompt at
		uint32_t clipBegin;		// Beginning of the clip
		uint32_t clipEnd;		// End of the clip
};

/// Curl helper function to handle the data it receives by passing it to the right Prompt instance
size_t write_data(void *buffer, size_t size, size_t nmemb, void *userp);
size_t write_data(void *buffer, size_t size, size_t nmemb, void *userp)
{
	Prompt *prompt = (Prompt*)userp;
	return prompt->writeData(buffer, size, nmemb);
}

/// HashMap of cached downloads, in order not to download them again
map<string, Prompt*>filesCache;
ost::Mutex filesCacheM;


/// Static Prompt for the BEEP
Prompt *beepPrompt;


/// A pointer to a single announcement frame in a Prompt file, complete with its duration in ms
class AnnouncementFrame : public gc {
	public:
		AnnouncementFrame(PromptInstance *prompt, int len, int64_t timestamp, uint32_t duration)
			{
				this->prompt = prompt;
				this->len = len;
				this->timestamp = timestamp;
				this->duration = duration;
			};
		~AnnouncementFrame() {};

		PromptInstance *getPromptInstance() { return prompt; };
		int getLength() { return len; };
		int64_t getTimestamp() { return timestamp; };
		uint32_t getDuration() { return duration; };
		void setDuration(uint32_t duration) { this->duration = duration; };

	private:
		PromptInstance *prompt;		// The Prompt instance this announcement belongs to
		int len;		// The length of the buffer to get
		int64_t timestamp;	// The position in the stream
		uint32_t duration;	// The duration in ms, needed for tempification and VCR
};
typedef list<AnnouncementFrame*> AnnouncementFrames;


/// Abstract base class for AudioFile (to write what's being recorded in realtime)
class RecordingFile : public gc {
	public:
		RecordingFile() {}
		virtual ~RecordingFile() {}

		void setDestination(string dest) { this->dest = dest; };
		string getDestination() { return dest; };
		string getFilename() { return filename; };
		string getPath() { return path; };
		virtual bool getRecordAudio() = 0;
		virtual string getType() = 0;
		virtual void writeFrame(MediaCtrlFrame *frame) = 0;

	protected:
		string filename;
		string path;
		string dest;
};
		
/// Helper class to write what's being recorded to a wav file in realtime
class AudioFile : public RecordingFile {
	public:
		AudioFile(string filename, string path, bool recordAudio);
		~AudioFile();

		bool prepare();
		bool getRecordAudio() { return rAudio; };
		string getType() { return "audio/x-wav"; };		// FIXME
		void writeFrame(MediaCtrlFrame *frame);

	private:
		FILE *file;
		bool prepared;
		bool rAudio;
};


/// Helper class for possible alternatives in a SRGS grammar
class SrgsAlternative : public gc {
	public:
		SrgsAlternative() {
			digits.clear();
			repeatMin = 1;
			repeatMax = 1;
		};
		~SrgsAlternative() {};
		
		list<string> digits;
		uint16_t repeatMin, repeatMax;
};
typedef list<SrgsAlternative*> SrgsAlternatives;

/// Helper class for SRGS rules
class SrgsRule : public gc {
	public:
		SrgsRule(string id, bool isPrivate) {
			this->id = id;
			privateScope = isPrivate;
			alternatives.clear();
		};
		~SrgsRule() {};

		string id;
		bool privateScope;
		SrgsAlternatives alternatives;
};


/// Extension to the base Stream class (msc-ivr)
class IvrStream : public MediaStream {
	public:
		IvrStream() {
			media = "";
			mediaType = MEDIACTRL_MEDIA_UNKNOWN;
			label = "";
			direction = SENDRECV;

			region = 0;
			priority = 15;	// FIXME Should be 100
		}
		~IvrStream() {};
	
		uint16_t region;
		uint16_t priority;
};


/// IvrPackage incoming CONTROL message: when parsing/preparing, it's a thread
class IvrMessage : public gc, public Thread {
	public:
		IvrMessage();
		~IvrMessage();

		void error(int code, string body="");

		bool scanonly;		// true if we are only scanning the XML
		bool stop;		// true if parsing is to be stopped
 		bool digging;		// true if only copying the <dialog> subelement for later parsing

		IvrPackage *pkg;
		string tid;
		string blob;
		uint32_t len;
		void *sender;

		string request;
		int level;
		list<string> childs;
		IvrDialog *dialog;
		string dialogId;
		bool newdialog;
		bool immediate;

		int conntype;		// Connection/Conference
		string connId;		// connectionid/conferenceid

		bool externalContent;	// Whether the <dialog>/<grammar> is externally referenced or not
		string content;		// Content of the <dialog>/<grammar> element containing the directives: this is either taken from the blob itself, or from a URI if externally referenced

		list<IvrStream *> streams;

		// Audit only
		bool auditCapabilities, auditDialogs;
		string auditDialog;

		// <grammar> validation only
		bool metaName, metaHttp, metaData, ruleFound;

	private:
		void run();
		bool running;
};

/// IVR Dialog class
class IvrDialog : public gc, public Thread {
	public:
		IvrDialog(IvrPackage *pkg, void *sender, string dialogId="");
		~IvrDialog();
		
		bool checkSender(void *requester) { return (requester == sender); };

		void setDefaults();
		int prepare();
		void setupStreams();
		void destroy(bool immediate, int exitstatus);
		void playAnnouncements();
		void playBeep();

		IvrPackage *getPackage() { return pkg; };

		void setTransactionId(string tid) { this->tid = tid; };
		void setConnectionId(string connectionId) { this->connectionId = connectionId; };
		void setConfId(string confId) { this->confId = confId; };
		void addModel(int model) { this->dlgModel |= model; };
		int getState() { return dlgState; };
		string getTransactionId() { return tid; };
		string getDialogId() { return dialogId; };
		int attachConnection();
		ControlPackageConnection *getConnection() { return connection; };
		string getConnectionId() { return connectionId; };
		string getConfId() { return confId; };
		int getModel() { return dlgModel; };
		string getModelString();
		uint16_t getTimeout() { return 15; };		// TODO Actually compute transaction timeout
		int addFilename(int how, string filename, uint32_t timeout, uint16_t soundLevel, uint32_t clipBegin, uint32_t clipEnd);

		void setIterations(uint32_t iterations) { this->iterations = iterations; };
		void setDuration(uint32_t duration) { this->duration = duration; };
		string checkDataParameter(int model, string name, string value, int *errorCode);
		void addRecordingType(string dest, bool audio, string type);

		void subscribe(bool notifyAll, bool notifyCollect, bool notifyVcr)
			{
				this->notifyAll = notifyAll;
				this->notifyCollect = notifyCollect;
				this->notifyVcr = notifyVcr;
			};
		void notifyEvent(string body);

		void incomingFrame(ControlPackageConnection *connection, ControlPackageConnection *subConnection, MediaCtrlFrame *frame);
		void incomingDtmf(ControlPackageConnection *connection, ControlPackageConnection *subConnection, int type);
		void connectionLocked(ControlPackageConnection *connection, ControlPackageConnection *subConnection);
		void connectionUnlocked(ControlPackageConnection *connection, ControlPackageConnection *subConnection);
		void connectionClosing(ControlPackageConnection *connection, ControlPackageConnection *subConnection);

		IvrPackage *pkg;

		list<IvrStream *> streams;
		stringstream errorString;	// Only needed when prepare() fails to describe the error to the AS

		int currentEndSync;		// Needed when adding parallel/sequential/normal filenames to <prompt>
		int currentSlot;		// Needed when adding parallel/sequential/normal filenames to <prompt>
		bool newTimelineStep, newSlot;

		// SRGS-related
		bool oneOf;
		string currentRule;
		int currentStep;
		string ruleToMatch;
		map<string, SrgsRule*>srgsRules;

	private:
		void run();

		TimerPort *timer;

		bool immediate, destroyDialog, running;

		void *sender;
		string tid;
		string dialogId;
		string connectionId, confId;
		int dlgModel;
		int dlgState;
		int dialogexitStatus;
		string dialogexitString;

		// Shared between templates
		bool done, playing, collecting, recording, restart;
		int termmode;	// What caused the termination of a dialog?
		uint32_t startTime;
		uint32_t iterations, currentIteration;			// positive integer
		uint32_t duration;					// time designation

		stringstream dialogInfo;	// All promptinfo, collectinfo etc goes here
		bool notifyAll;

		// <prompt> --> p*
		list<int>endsync;
		list<PromptInstance*> audioPrompts;
		list<TimeLine*> audioTimeLine[TRACKS];	// We support TRACKS (default is 4) audio tracks at max (which means, 4 parallel playbacks for the same dialog)
		AnnouncementFrames *audioAnnouncements[TRACKS];
		uint16_t audioAnnouncementsSize[TRACKS];
		uint32_t audioDuration[TRACKS];
		uint32_t aTiming;
		bool pAudio;		// Should we play audio? (this is affected both by the list of media and by the list of streams...)
		string pBase;						// string
		bool pBargein;						// boolean
		list<int>pVcrEvents;

		// <collect> --> c*
		string dtmfString;
		ost::Mutex mTones;
		DtmfTones tones;
		bool cCleardigitbuffer;					// boolean
		int cEscapekey, cTermchar;				// DTMF digit
		uint32_t cTimeout, cInterdigittimeout, cTermtimeout;	// time designation
		uint16_t cMaxdigits;					// positive integer
		bool notifyCollect;
		bool cMatch;

		// <control> --> v*
		uint32_t vSkipinterval, vPauseinterval;			// time designation
		int vVolumeinterval, vSpeedinterval;			// percentage
		int vStartkey, vEndkey, vFfkey, vRwkey, vPausekey, vResumekey,
			vVolupkey, vVoldownkey, vSpeeddownkey, vSpeedupkey;	// DTMF digit
		bool vcrMode;
		string vcrInfo;		// List of <controlmatch> elements
		bool notifyVcr;
		// TODO We don't support <grammar> yet

		// <record> --> r*
		bool rAudio;		// Should we record audio? (this is affected both by rType and by the list of streams...)
		bool rInputreceived;
		uint32_t rTimeout;					// time designation
		string rType;						// string
		list<RecordingFile*> rDests;
		bool rVadinitial, rVadfinal, rDtmfterm, rBeep;		// boolean
		uint32_t rMaxtime, rFinalsilence, rSilencestarttime;			// time designation
		TimerPort *rSilencetimer;
		bool rAppend;						// boolean
		MediaCtrlFrames *beepFrames;

		ControlPackageConnection *connection;
};

/// IVR (msc-ivr) package cass
class IvrPackage : public ControlPackage {
	public:
		IvrPackage();
		~IvrPackage();

		bool setup();
		string getInfo();

		ControlPackageConnection *attach(IvrDialog *dlg, string connectionId);
		void detach(IvrDialog *dlg, ControlPackageConnection *connection);

		void control(void *sender, string tid, string blob);

		void setCollector(void *frameCollector);

		void sendFrame(ControlPackageConnection *connection, MediaCtrlFrame *frame);
		void clearDtmfBuffer(ControlPackageConnection *connection);
		int getNextDtmfBuffer(ControlPackageConnection *connection);
		void incomingFrame(ControlPackageConnection *connection, ControlPackageConnection *subConnection, MediaCtrlFrame *frame);
		void incomingDtmf(ControlPackageConnection *connection, ControlPackageConnection *subConnection, int type);
		void frameSent(ControlPackageConnection *connection, ControlPackageConnection *subConnection, MediaCtrlFrame *frame)
			{
				return;
			};
		void connectionLocked(ControlPackageConnection *connection, ControlPackageConnection *subConnection);
		void connectionUnlocked(ControlPackageConnection *connection, ControlPackageConnection *subConnection);
		void connectionClosing(ControlPackageConnection *connection, ControlPackageConnection *subConnection);

		void handleControl(IvrMessage *msg);
		void deleteMessage(IvrMessage *msg);
		void endDialog(string dialogId);

		string webAddress;
		string webPort;
		string webPath, promptsPath, recordingsPath, tmpPath;

		list<IvrMessage *> messages;
		map<string, IvrDialog *>dialogs;
		map<string, IvrDialog *>connections;
		list<string>endedDialogs;
		ost::Mutex mEnded;

	private:
		void run();
		bool alive;
		list<IvrMessage *> endedMessages;
};


// Prompt handler
Prompt::Prompt(string name, string tmpFolder)
{
	cout << "[IVR] Creating CURL handler for new prompt: " << name << endl;
	this->name = name;
	handle = curl_easy_init();
	if(handle) {
		curl_easy_setopt(handle, CURLOPT_URL, name.c_str());
		curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, write_data);
		curl_easy_setopt(handle, CURLOPT_WRITEDATA, this);
		curl_easy_setopt(handle, CURLOPT_VERBOSE, 1);
	}
	file = NULL;
	if(tmpFolder == "")
		tmpFolder = "./tmp";
	filename = tmpFolder + "/" + random_string(16);
	string::size_type end = name.rfind(".");
	if(end == string::npos) {	// FIXME take care of mime
		if(handle)
			curl_easy_cleanup(handle);
	}
	string extension = name.substr(end);
	filename += extension;
	cout << "[IVR] \t\tFile will be saved in: " << filename << endl;
	// fetchtimeout
	timeout = 30000;
	timedOut = false;
	timer = NULL;
	startTime = 0;
	status = PROMPT_NONE;
}

Prompt::~Prompt()
{
	status = PROMPT_INVALID;
	cout << "[IVR] Destroying Prompt: " << name << endl;
	if(file)
		fclose(file);
	if(handle)
		curl_easy_cleanup(handle);
}

int Prompt::startTransfer(uint32_t timeout)
{
	status = PROMPT_DOWNLOADING;
	if(!handle) {
		status = PROMPT_INVALID;
		cond.signal(true);
		return -1;
	}
	this->timeout = timeout;
	// TODO If the URL is file://*** we should just open the file, and not copy it
	file = fopen(filename.c_str(), "wb");
	if(!file) {
		cout << "[IVR] Couldn't open " << filename << " for writing (a permissions problem?)" << endl;
		status = PROMPT_INVALID;
		cond.signal(true);
		return -1;
	}
	cout << "[IVR] Starting transfer for " << filename << " (fetchtimeout=" << dec << timeout << "ms)" << endl;
	// Start the timeout timer
	timer = new TimerPort();
	timer->incTimer(0);
	startTime = timer->getElapsed();
	// Start the download
	int err = curl_easy_perform(handle);
	fclose(file);
	file = NULL;
	long int code = 0;
	curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &code);
	curl_easy_cleanup(handle);
	if(code >= 400)
		err = code;
	handle = NULL;
	delete timer;
	timer = NULL;
	if(timedOut) {
		status = PROMPT_INVALID;
		cond.signal(true);
		return -100;	// FIXME
	}
	if(err != 0) {
		status = PROMPT_INVALID;
		cond.signal(true);
		return err;
	}
	status = PROMPT_RETRIEVED;
	cond.signal(true);
	return 0;
}

size_t Prompt::writeData(void *buffer, size_t size, size_t nmemb)
{
	if((timer->getElapsed()-startTime) > timeout) {		// FIXME This check should be outside the prompt, to avoid locking DNS lookups
		cout << "[IVR] Exceeded fetchtimeout (" << dec << timeout << "ms) for " << filename << "!" << endl;
		timedOut = true;
		return 0;
	}
	if(fwrite(buffer, size, nmemb, file) != nmemb) {
		cout << "[IVR] Couldn't write on " << filename << " (a permissions problem?)" << endl;
		return 0;
	}
	fflush(file);
	return nmemb;
}

int Prompt::wait()
{
	cout << "[IVR] Waiting for Prompt '" << name << "' to complete..." << endl;
	cond.wait();
	return status;
}


FILE *PromptInstance::openFile()
{
	if(file)
		return file;
	if(prompt == NULL)
		return NULL;	// FIXME
	file = fopen(prompt->getFilename().c_str(), "rb");
	return file;
}

void PromptInstance::closeFile()
{
	if(file)
		fclose(file);
	file = NULL;
}

// ControlPackage Class Factories
extern "C" ControlPackage* create(ControlPackageCallback *callback)
{
	IvrPackage *pkg = new IvrPackage();
	pkg->setCallback(callback);
	return pkg;
}

extern "C" void destroy(ControlPackage* p)
{
	delete p;
}


AudioFile::AudioFile(string filename, string path, bool recordAudio)
{
	cout << "[IVR] Allocating AudioFile: " << filename << endl;
	this->filename = filename;
	this->path = path;
	rAudio = recordAudio;
	prepared = false;
	file = NULL;
	dest = "";
}

AudioFile::~AudioFile()
{
	cout << "[IVR] Closing AudioFile: " << filename << endl;
	if(!prepared)
		return;
	fclose(file);
}

bool AudioFile::prepare()
{
	cout << "[IVR] Preparing AudioFile: " << filename << endl;
	file = fopen(path.c_str(), "wb");
	if(file == NULL)
		return false;	// Couldn't open the file
	wavHdr header = {
		{'R', 'I', 'F', 'F'},
		0,
		{'W', 'A', 'V', 'E'},
		{'f', 'm', 't', ' '},
		16,
		1,
		1,
		8000,
		16000,
		2,
		16,
		{'d', 'a', 't', 'a'},
		0
	};
	if(fwrite(&header, 1, sizeof(header), file) != sizeof(header))
		cout << "[IVR] \tCouldn't write header! (expect problems...)" << endl;
	fflush(file);
	cout << "[IVR] \tOpened " << filename << " for the recording (audio)" << endl;
	prepared = true;
	return true;
}

void AudioFile::writeFrame(MediaCtrlFrame *frame)
{
	if(frame == NULL)
		return;
	if(!prepared || (file == NULL) || !rAudio || (frame->getMediaType() != MEDIACTRL_MEDIA_AUDIO)) {
		return;
	}
	fseek(file, 0, SEEK_END);
	if(fwrite(frame->getBuffer(), sizeof(uint8_t), frame->getLen(), file) != (uint16_t)frame->getLen()) {
		cout << "[IVR] Couldn't write on file (a permissions problem?)" << endl;
		fclose(file);
		file = NULL;
		return;
	}
	// Update the header
	fseek(file, 0, SEEK_END);
	long int size = ftell(file) - 8;
	fseek(file, 4, SEEK_SET);
	fwrite(&size, sizeof(uint32_t), 1, file);
	size += 8;
	fseek(file, 40, SEEK_SET);
	fwrite(&size, sizeof(uint32_t), 1, file);
	fflush(file);
}


// The IvrMessage Class (used for transactions handling)
IvrMessage::IvrMessage()
{
	scanonly = false;
	stop = false;
	immediate = false;
	digging = false;
	pkg = NULL;
	tid = "";
	blob = "";
	sender = NULL;
	len = 0;
	level = 0;
	childs.clear();
	dialog = NULL;
	dialogId = "";
	newdialog = true;
	streams.clear();
	running = false;

	auditCapabilities = true;
	auditDialogs = true;
	auditDialog = "";

	metaName = false;
	metaHttp = false;
	metaData = false;
	ruleFound = false;
}

IvrMessage::~IvrMessage()
{
	if(running)
		join();
	if(!streams.empty()) {
		while(!streams.empty()) {
			IvrStream *stream = streams.front();
			streams.pop_front();
			delete stream;
			stream = NULL;
		}
	}
}

void IvrMessage::error(int code, string body)
{
	cout << "[IVR] Error triggered (" << dec << code << ", " << body << ") at level " << dec << level << endl;
	stop = true;
	string reason;
	switch(code) {
		case 200:
			reason = "OK";
			break;
		case 400:
			reason = "Syntax error";
			break;
		case 401:
			reason = "";	// Reserved for future use
			break;
		case 402:
			reason = "";	// Reserved for future use
			break;
		case 403:
			reason = "";	// Reserved for future use
			break;
		case 404:
			reason = "";	// Reserved for future use
			break;
		case 405:
			reason = "dialogid already exists";
			break;
		case 406:
			reason = "dialogid does not exist";
			break;
		case 407:
			reason = "connectionid does not exist";
			break;
		case 408:
			reason = "conferenceid does not exist";
			break;
		case 409:
			reason = "Resource cannot be retrieved";
			break;
		case 410:
			reason = "Dialog execution canceled";
			break;
		case 411:
			reason = "Incompatible stream configuration";
			break;
		case 412:
			reason = "Media stream not available";
			break;
		case 413:
			reason = "Control keys with same value";
			break;
		case 414:
			reason = "";	// Reserved for future use
			break;
		case 415:
			reason = "";	// Reserved for future use
			break;
		case 416:
			reason = "";	// Reserved for future use
			break;
		case 417:
			reason = "";	// Reserved for future use
			break;
		case 418:
			reason = "";	// Reserved for future use
			break;
		case 419:
			reason = "Other execution error";
			break;
		case 420:
			reason = "Unsupported URI scheme";
			break;
		case 421:
			reason = "Unsupported dialog language";
			break;
		case 422:
			reason = "Unsupported playback format";
			break;
		case 423:
			reason = "Unsupported record format";
			break;
		case 424:
			reason = "Unsupported grammar format";
			break;
		case 425:
			reason = "Unsupported variable configuration";
			break;
		case 426:
			reason = "Unsupported DTMF configuration";
			break;
		case 427:
			reason = "Unsupported parameter";
			break;
		case 428:
			reason = "Unsupported media stream configuration";
			break;
		case 429:
			reason = "Unsupported playback configuration";
			break;
		case 430:
			reason = "Unsupported record configuration";
			break;
		case 431:
			reason = "Unsupported foreign namespace attribute or element";
			break;
		case 432:
			reason = "Unsupported multiple dialog capability";
			break;
		case 433:
			reason = "Unsupported collect and record configuration";
			break;
		case 434:
			reason = "Unsupported VAD capability";
			break;
		case 435:
			reason = "Unsupported parallel playback";
			break;
		case 436:
			reason = "";	// Reserved for future use
			break;
		case 437:
			reason = "";	// Reserved for future use
			break;
		case 438:
			reason = "";	// Reserved for future use
			break;
		case 439:
			reason = "Other unsupported capability";
			break;
		default:
			reason = "Other error";
			break;
	}
	stringstream newblob;
	newblob << "<mscivr version=\"1.0\" xmlns=\"urn:ietf:params:xml:ns:msc-ivr\">";
	newblob << "<response status=\"";
	newblob << code << "\"";
	newblob << " dialogid=\"";
	if(dialog != NULL)
		newblob << dialog->getDialogId();		// FIXME dialogid is mandatory
	newblob << "\"";
	newblob << " reason=\"" << reason;
	if(body != "")
		newblob << ": " << body;
	newblob << "\"/></mscivr>";
	pkg->callback->report(pkg, sender, tid, 200, 10, newblob.str());
	if(newdialog && dialog) 	// Remove the newly created dialog if there was an error
		delete dialog;
}

void IvrMessage::run()
{
	running = true;
	scanonly = false;
	content = "";
	externalContent = false;
	digging = false;
	XML_Parser parser = XML_ParserCreate(NULL);
	XML_SetUserData(parser, this);
	XML_SetElementHandler(parser, startElement, endElement);
	XML_SetCharacterDataHandler(parser, valueElement);
	XML_Parse(parser, blob.c_str(), blob.length(), 1);
	XML_ParserFree(parser);
	pkg->deleteMessage(this);
	running = false;
}


// The Dialog Class
IvrDialog::IvrDialog(IvrPackage *pkg, void *sender, string dialogId)
{
	this->pkg = pkg;
	this->sender = sender;
	if(dialogId == "") {	// Generate a random one
		while(1) {	// which does not conflict
			dialogId = random_string(8);
			if(pkg->dialogs.find(dialogId) == pkg->dialogs.end())
				break;
		}
	}
	this->dialogId = dialogId;
	cout << "[IVR] New IvrDialog created: " << dialogId << endl;
	cout << endl;
	dlgState = DIALOG_IDLE;
	dlgModel = 0;		// No model yet
	dialogexitStatus = -1;	// No dialogexit value yet
	tid = "";
	connectionId = "";
	confId = "";
	connection = NULL;

	immediate = destroyDialog = running = false;
	beepFrames = NULL;

	setDefaults();
}

IvrDialog::~IvrDialog()
{
	cout << "[IVR] Destroying IvrDialog: " << dialogId << endl;
	if(connection != NULL)
		pkg->detach(this, connection);
	if(timer)
		delete timer;
	timer = NULL;

	if(!streams.empty()) {
		while(!streams.empty()) {
			IvrStream *stream = streams.front();
			streams.pop_front();
			if(stream != NULL)
				delete stream;
			stream = NULL;
		}
	}

	if(!rDests.empty()) {
		while(!rDests.empty())
			rDests.pop_front();
	}

	// Free the frames
	if(beepFrames != NULL) {
#if 0
		MediaCtrlFrame *frame = NULL;
		while(!beepFrames->empty()) {
			frame = beepFrames->front();
			beepFrames->pop_front();
			frame = NULL;
		}
#endif
		delete beepFrames;	// FIXME
		beepFrames = NULL;
	}
	AnnouncementFrame *annc = NULL;
	int i=0;
	for(i=0; i < TRACKS; i++) {
		if(audioAnnouncements[i] != NULL) {
			cout << "[IVR] Freeing audio announcements (track=" << dec << i << ")" << endl;
			while(!audioAnnouncements[i]->empty()) {
				annc = audioAnnouncements[i]->front();
				audioAnnouncements[i]->pop_front();
				if(annc != NULL)
					delete annc;
				annc = NULL;
			}
			delete audioAnnouncements[i];
			audioAnnouncements[i] = NULL;
		}
		if(!audioTimeLine[i].empty()) {
			while(!audioTimeLine[i].empty()) {
				TimeLine *audioTL = audioTimeLine[i].front();
				audioTimeLine[i].pop_front();
				if(audioTL != NULL)
					delete audioTL;
				audioTL = NULL;
			}
		}
	}
	if(!audioPrompts.empty()) {
		while(!audioPrompts.empty()) {
			PromptInstance *instance = audioPrompts.front();
			audioPrompts.pop_front();
			if(instance != NULL)
				delete instance;
			instance = NULL;
		}
	}
}

void IvrDialog::setDefaults()
{
	// Shared
	playing = collecting = recording = restart = done = false;
	termmode = TERMMODE_UNKNOWN;
	startTime = 0;
	timer = NULL;
	iterations = 1;
	duration = 0;	// No limits
	notifyAll = false;	// Default is true, but only when subscribed

	// <prompt>
	pAudio = false;
	currentEndSync = ENDSYNC_NONE;
	currentSlot = 0;
	newTimelineStep = newSlot = true;
	endsync.clear();
	audioPrompts.clear();
	int i=0;
	for(i=0; i<TRACKS; i++) {
		audioTimeLine[i].clear();
		audioAnnouncements[i] = NULL;
		audioAnnouncementsSize[i] = 0;
		audioDuration[i] = 0;
	}
	aTiming;
	pBase = "";
	pBargein = true;
	pVcrEvents.clear();

	// <collect>
	dtmfString = "";
	tones.clear();
	cCleardigitbuffer = true;
	cTimeout = 5000;
	cInterdigittimeout = 2000;
	cTermtimeout = 0;
	cMaxdigits = 5;
	cEscapekey = MEDIACTRL_DTMF_NONE;
	cTermchar = MEDIACTRL_DTMF_POUND;
	notifyCollect = false;
	cMatch = false;
	// SRGS only
	oneOf = false;
	currentRule = "";
	ruleToMatch = "";
	currentStep = 0;
	srgsRules.clear();

	// <control>
	vSkipinterval = 6000;
	vPauseinterval = 10000;
	vVolumeinterval = 10;
	vStartkey = MEDIACTRL_DTMF_NONE;
	vEndkey = MEDIACTRL_DTMF_NONE;
	vFfkey = MEDIACTRL_DTMF_NONE;
	vRwkey = MEDIACTRL_DTMF_NONE;
	vPausekey = MEDIACTRL_DTMF_NONE;
	vResumekey = MEDIACTRL_DTMF_NONE;
	vVolupkey = MEDIACTRL_DTMF_NONE;
	vVoldownkey = MEDIACTRL_DTMF_NONE;
	vSpeedupkey = MEDIACTRL_DTMF_NONE;
	vSpeeddownkey = MEDIACTRL_DTMF_NONE;
	vcrMode = false;
	notifyVcr = false;
	vcrInfo = "";

	// <record>
	rAudio = rInputreceived = false;
	rTimeout = 5000;
	rType = "";	// FIXME
	rDests.clear();
	rVadinitial = false;
	rVadfinal = false;
	rDtmfterm = true;
	rBeep = false;
	rMaxtime = 15000;
	rFinalsilence = 5000;
	rSilencestarttime = 0;
	rSilencetimer = NULL;
	rAppend = false;
}

int IvrDialog::prepare()
{
	dlgState = DIALOG_PREPARING;
	// The XML and <dialog> have aready been validated, just do the final stuff
	if(endsync.empty()) {
		cout << "[IVR] No TimeLine steps (playback is disabled)" << endl;
	} else {	// Retrieve prompts
		string filename;
		int endSync = -1;
		TimeLine *audioTL[TRACKS], *tempTL = NULL;
		if(endsync.empty())
			cout << "[IVR] No TimeLine steps (playback is disabled)" << endl;
		else
			cout << "[IVR] " << dec << endsync.size() << " TimeLine steps available" << endl;
		int step = 0;
		while(!endsync.empty()) {
			step++;
			cout << "[IVR] *** Preparing TimeLine step " << dec << step << "..." << endl;
			endSync = endsync.front();
			if(endSync == ENDSYNC_NONE)
				cout << "[IVR] \tNot a parallel TimeLine" << endl;
			else if(endSync == ENDSYNC_FIRST)
				cout << "[IVR] \tParallel TimeLine (endsync=first)" << endl;
			else if(endSync == ENDSYNC_LAST)
				cout << "[IVR] \tParallel TimeLine (endsync=last)" << endl;
			int i=0;
			for(i=0; i<TRACKS; i++) {
				audioTL[i] = audioTimeLine[i].front();
				cout << "[IVR] \t\t " << dec << audioTL[i]->pFilenames.size() << " audio filenames (track=" << dec << i << ")" << endl;
			}
			uint32_t audioDurations = 0, audioShortestDuration = 0;
			for(i=0; i<TRACKS; i++)
				audioDuration[i] = 0;
			int step = -1;
			while(1) {
				step++;
				if(step == TRACKS)	// TRACKS(4?) Audio
					break;
				tempTL = audioTL[step];
				if(!tempTL->pFilenames.empty()) {
					while(!tempTL->pFilenames.empty()) {
						string filenameFull = tempTL->pFilenames.front();
						if(pBase == "")	// No base url
							filename = tempTL->pFilenames.front();
						else {
							regex re;
							re.assign("(ftp|http|https|file):\\/\\/(\\S+)", regex_constants::icase);
							if(regex_match(filename.c_str(), re))
								filename = tempTL->pFilenames.front();
							else {	// Only use 'base' if it isn't a real URL
								filename = pBase;
								filename.append(tempTL->pFilenames.front());
							}
						}
						// Check if it's a valid url (FIXME)
						regex re;
						// TODO The list should be larger: cURL supports much more than that...
						re.assign("(ftp|http|https|file):\\/\\/(\\S+)", regex_constants::icase);	// FIXME Better handling of URL validation (e.g. passwords)
						if(!regex_match(filename.c_str(), re)) {
							cout << "[IVR]     Invalid URL: " << filename.c_str() << endl;
							errorString << "Invalid URL: " << filename.c_str();
							return 420;
						}
						Prompt *prompt = NULL;
						// Check the cache first
						filesCacheM.enter();
						if(filesCache.find(filename) != filesCache.end()) {	// Use the cache
							prompt = filesCache[filename];
							filesCacheM.leave();
							// Check the state of the Prompt, and wait for its completion if needed
							if(prompt->getStatus() == PROMPT_DOWNLOADING) {
								// Another dialog is downloading the same prompt, let's wait for it to complete and use it together
								if(prompt->wait() != PROMPT_RETRIEVED) {
									cout << "[IVR]     Couldn't retrieve resource " << filename.c_str() << endl;
									errorString << "Error for " << filename.c_str();
									return 409;
								}
							}
							cout << "[IVR]     Opened/Got prompt file " << filename.c_str() << endl;
							cout << "[IVR]        Using the cached tmp file: " << prompt->getFilename() << endl;
						} else {	// Download the file and cache it
							prompt = new Prompt(filename, pkg->tmpPath);
							// Cache the download to begin with
							cout << "[IVR]        Caching the tmp file: " << prompt->getFilename() << endl;
							filesCache[filename] = prompt;
							filesCacheM.leave();
							int err = prompt->startTransfer(tempTL->pTimeouts.front());
							if(err != 0) {
								if(err < 0) {
									cout << "[IVR]     Couldn't open/get file " << filename.c_str() << endl;
									errorString << "Couldn't open/get file " << filename.c_str();
								} else {
									cout << "[IVR]     Couldn't retrieve resource " << filename.c_str() << endl;
									errorString << "Error " << dec << err << " for " << filename.c_str();
								}
								filesCacheM.enter();
								filesCache.erase(filename);
								filesCacheM.leave();
								delete prompt;
								return 409;
							}
							cout << "[IVR]     Opened/Got prompt file " << filename.c_str() << endl;
						}
						tempTL->pFilenames.pop_front();
						uint32_t timeoutValue = tempTL->pTimeouts.front();
						tempTL->pTimeouts.pop_front();
						// Create a new PromptInstance and set the clip settings
						PromptInstance *promptInstance = new PromptInstance(prompt);
						uint16_t soundLevel = tempTL->pSoundlevel.front();
						tempTL->pSoundlevel.pop_front();
						promptInstance->setSoundLevel(soundLevel);
						cout << "[IVR]     Set volume to " << dec << soundLevel << "%" << endl;				
						uint32_t clipBegin = tempTL->pClipbegin.front();
						uint32_t clipEnd = tempTL->pClipend.front();
						tempTL->pClipbegin.pop_front();
						tempTL->pClipend.pop_front();
						promptInstance->setClipBounds(clipBegin, clipEnd);
						cout << "[IVR]     Set clipBegin to ";
						if(clipBegin > 0)
							cout << dec << clipBegin << "ms, ";
						else
							cout << "(start), ";
						cout << "clipEnd to ";
						if(clipEnd > 0)
							cout << dec << clipEnd << "ms" << endl;
						else
							cout << "(end)" << endl;
						cout << "[IVR] Preparing audio frames now" << endl;
						AVFormatContext *fctx = NULL;
						if(av_open_input_file(&fctx, prompt->getFilename().c_str(), NULL, 0, NULL) != 0) {
							cout << "[IVR] Couldn't open the file " << prompt->getFilename() << endl;
							// Remove the invalid prompt from the cache
							filesCacheM.enter();
							filesCache.erase(prompt->getFilename());
							filesCacheM.leave();
							errorString << "Couldn't open the file " << prompt->getName();
							delete promptInstance;
							delete prompt;
							return 429;		// FIXME This is an error opening, not retrieving...
						}
						if(av_find_stream_info(fctx) < 0) {
							cout << "[IVR] Couldn't find stream information for the file " << prompt->getFilename() << endl;
							// Remove the invalid prompt from the cache
							filesCacheM.enter();
							filesCache.erase(prompt->getFilename());
							filesCacheM.leave();
							errorString << "Couldn't find stream information for the file " << prompt->getName();
							delete promptInstance;
							delete prompt;
							av_close_input_file(fctx);
							return 429;		// FIXME This is an error opening, not retrieving...
						}
						dump_format(fctx, 0, prompt->getFilename().c_str(), 0);
						if(fctx->nb_streams < 1) {
							cout << "[IVR] No stream available for the file " << prompt->getFilename() << endl;
							// Remove the invalid prompt from the cache
							filesCacheM.enter();
							filesCache.erase(prompt->getFilename());
							filesCacheM.leave();
							errorString << "No stream available for the file " << prompt->getName();
							delete promptInstance;
							delete prompt;
							av_close_input_file(fctx);
							return 429;		// FIXME This is an error opening, not retrieving...
						}
						int i = 0;
						for(i=0; i<fctx->nb_streams; i++) {
							int width = 0, height = 0, pix_fmt = 0;
							if(av_seek_frame(fctx, -1, 0, AVSEEK_FLAG_BACKWARD) < 0) {
								cout << "[IVR]\t\tCouldn't seek backward for stream " << dec << i << endl;
								// Remove the invalid prompt from the cache
								filesCacheM.enter();
								filesCache.erase(prompt->getFilename());
								filesCacheM.leave();
								errorString << "Error seeking file " << prompt->getName();
								delete promptInstance;
								delete prompt;
								av_close_input_file(fctx);
								return 429;		// FIXME This is an error opening, not retrieving...
							}
							AVCodecContext *ctx = fctx->streams[i]->codec;
							if(ctx->codec_type == CODEC_TYPE_VIDEO) {
								if(step > 0)	// We're looking for audio, try the next track
									continue;
							}
							AVCodec *codec = avcodec_find_decoder(ctx->codec_id);
							if(!codec) {
								if(pAudio && (ctx->codec_type == CODEC_TYPE_AUDIO)) {	// We explicitly need audio and it doesn't work
									// Remove the invalid prompt from the cache
									filesCacheM.enter();
									filesCache.erase(prompt->getFilename());
									filesCacheM.leave();
									errorString << "Error opening codec for file " << prompt->getName();
									delete promptInstance;
									delete prompt;
									av_close_input_file(fctx);
									return 429;		// FIXME This is an error opening, not retrieving...
								} else
									continue;
							}
							if(avcodec_open(ctx, codec) < 0) {
								cout << "[IVR]\t\tCouldn't initiate the codec for stream " << dec << i << endl;
								if(pAudio && (ctx->codec_type == CODEC_TYPE_AUDIO)) {	// We need audio and it doesn't work
									// Remove the invalid prompt from the cache
									filesCacheM.enter();
									filesCache.erase(prompt->getFilename());
									filesCacheM.leave();
									errorString << "Error opening codec for file " << prompt->getName();
									delete promptInstance;
									delete prompt;
									av_close_input_file(fctx);
									return 429;		// FIXME This is an error opening, not retrieving...
								} else
									continue;
							}
							cout << "[IVR] Track #" << dec << i << ", " << fctx->streams[i]->nb_frames << " frames" << endl;
							// Actually process audio track
							if(audioAnnouncements[step] == NULL) {
								audioAnnouncements[step] = new AnnouncementFrames;
								audioAnnouncements[step]->clear();
								audioAnnouncementsSize[step] = 0;
							}
							// Try to guess rate and timing
							int mins, secs, usecs;
							secs = fctx->duration / AV_TIME_BASE;
							usecs = fctx->duration % AV_TIME_BASE;
							mins = secs / 60;
							secs %= 60;
							uint32_t duration = 1000*(mins*60+secs) + usecs/1000;
							float rate = 0;
							if((duration != 0) && (fctx->streams[i]->nb_frames != 0)) {
								rate = (float)fctx->streams[i]->nb_frames/((float)duration/1000);
								aTiming = (1000*duration)/fctx->streams[i]->nb_frames;
							} else {
								// We'll check it later
								rate = 0;
								aTiming = 0;
							}
							if(rate > 0)
								cout << "[IVR] Duration: " << dec << duration << "ms (rate ~ " << rate << ", timing ~ " << aTiming << ") in " << dec << fctx->streams[i]->nb_frames << " frames" << endl;
							// Check if we can do resampling
							ReSampleContext *resample = audio_resample_init(1, ctx->channels, 8000, ctx->sample_rate);
							if(!resample) {
								cout << "[IVR] \t\tCouldn't create the resampler for stream " << dec << i << "..." << endl;
								errorString << "Error creating resampler for file " << prompt->getName();
								filesCacheM.enter();
								filesCache.erase(prompt->getFilename());
								filesCacheM.leave();
								delete promptInstance;
								delete prompt;
								avcodec_close(ctx);
								av_close_input_file(fctx);
								return 429;		// FIXME This is an error opening, not retrieving...
							}
							audio_resample_close(resample);
							resample = NULL;
							// Start working on the indexing
							AVPacket packet;
							packet.data = NULL;
							int j = 0;
							AnnouncementFrame *frame = NULL;
							uint32_t currentMs = 0, newMs = 0;	// To be used in case a clipBegin and clipEnd are involved
							uint32_t clipBegin = promptInstance->getClipBegin();
							uint32_t clipEnd = promptInstance->getClipEnd();
//									rate = 0; aTiming = 0;	// FIXME
							while(1) {
								if(packet.data)
									av_free_packet(&packet);
								if(av_read_frame(fctx, &packet) < 0) {
									if(packet.data)
										av_free_packet(&packet);
									break;
								}
								if(packet.stream_index != i)
									continue;
								if(clipBegin > 0) {
									currentMs = 1000*(packet.pts*(fctx->streams[i]->time_base.num))/(fctx->streams[i]->time_base.den);
									if(clipBegin > currentMs)
										continue;
									if(clipEnd <= currentMs)
										break;
								}
								frame = new AnnouncementFrame(promptInstance, packet.size, packet.pts, aTiming);
								audioDuration[step] += aTiming;
								audioAnnouncements[step]->push_back(frame);
								j++;
								if(fctx->streams[i]->nb_frames == j) {
									if(packet.data)
										av_free_packet(&packet);
									break;
								}
							}
							if(audioAnnouncements[step]->size() == audioAnnouncementsSize[step]) {
								cout << "[IVR] \t\t\tNo audio frames (slot=" << dec << step << ") could be indexed" << endl;
								// Remove the invalid prompt from the cache
								errorString << "Error indexing audio frames for file " << prompt->getName();
								filesCacheM.enter();
								filesCache.erase(prompt->getFilename());
								filesCacheM.leave();
								delete promptInstance;
								delete prompt;
								avcodec_close(ctx);
								av_close_input_file(fctx);
								return 429;		// FIXME This is an error opening, not retrieving...
							}
							if(rate == 0) {	// No fps were guessed before, try now
								if(promptInstance->getClipBegin() > 0)
									duration = promptInstance->getClipEnd() - promptInstance->getClipBegin();
								if(duration > 1000) {	// FIXME
									rate = (float)(audioAnnouncements[step]->size()-audioAnnouncementsSize[step])/((float)duration/1000);
									aTiming = (1000*duration)/(audioAnnouncements[step]->size()-audioAnnouncementsSize[step]);
								} else {	// FIXME Assume 50 samples (aTiming 20ms)
									rate = 50;
									aTiming = 20000;
									duration = 1000*(audioAnnouncements[step]->size()-audioAnnouncementsSize[step])/50;
								}
								cout << "[IVR] Duration: " << dec << duration << "ms (rate ~ " << rate << ", timing ~ " << aTiming << ") in " << dec << (audioAnnouncements[step]->size()-audioAnnouncementsSize[step]) << " frames" << endl;
								// Update all the aTimings accordingly
								list<AnnouncementFrame*>::iterator aIter;
								for(aIter = audioAnnouncements[step]->begin(); aIter != audioAnnouncements[step]->end();  aIter++) {
									if((*aIter) == NULL)
										continue;
									if((*aIter)->getDuration() == 0) {
										(*aIter)->setDuration(aTiming);
										audioDuration[step] += aTiming;
									}
								}
							}
							audioAnnouncementsSize[step] = audioAnnouncements[step]->size();
							if(audioDurations < audioDuration[step])
								audioDurations = audioDuration[step];
							if((audioShortestDuration == 0) || (audioShortestDuration > audioDuration[step]))
								audioShortestDuration = audioDuration[step];
							avcodec_close(ctx);
						}
						av_close_input_file(fctx);
						fctx = NULL;
						audioPrompts.push_back(promptInstance);
						promptInstance->closeFile();
					}
				}
			}
			cout << "[IVR] TimeLine: audio=" << dec << audioDurations << "ms (shortest=" << dec << audioShortestDuration << "ms)" << endl;
			// Sync the parallel audio tracks with each other
			for(i=0; i<TRACKS; i++) {
				if(audioAnnouncements[i] == NULL)
					continue;
				if(endSync == ENDSYNC_FIRST) {
					if(audioDuration[i] > audioShortestDuration) {
						uint32_t diff = audioDuration[i] - audioShortestDuration;
						uint16_t pad = 0;
						while(1) {
							if(diff < 10000)
								break;
							pad++;
							AnnouncementFrame *audioFrame = audioAnnouncements[i]->back();
							audioAnnouncements[i]->pop_back();
							delete audioFrame;
							if(diff < 20000)
								break;
							diff = diff - 20000;							
						}
					} else if(audioDuration[i] == 0) {
						uint32_t diff = audioShortestDuration;
						uint16_t pad = 0;
						while(1) {
							if(diff < 10000)
								break;
							pad++;
							audioAnnouncements[i]->push_back(new AnnouncementFrame(NULL, 0, 0, 20000));
							if(diff < 20000)
								break;
							diff = diff - 20000;
						}
					}
					audioDuration[i] = audioShortestDuration;
					audioDurations = audioShortestDuration;
				} else {
					if(audioDuration[i] < audioDurations) {
						uint32_t diff = audioDurations - audioDuration[i];
						uint16_t pad = 0;
						while(1) {
							if(diff < 10000)
								break;
							pad++;
							audioAnnouncements[i]->push_back(new AnnouncementFrame(NULL, 0, 0, 20000));
							if(diff < 20000)
								break;
							diff = diff - 20000;
						}
					}
					audioDuration[i] = audioDurations;
				}
			}
			cout << "[IVR] Streams synced... TimeLine: audio=" << dec << audioDurations << "ms" << endl;
			endsync.pop_front();
			for(i=0; i<TRACKS; i++) {
				audioTimeLine[i].pop_front();	// TODO free timelines...
				delete audioTL[i];
				audioTL[i] = NULL;
			}
		}
	}
	// If we're going to record, open the file: a failure to do so would be an error
	if(dlgModel & MODEL_RECORD) {
		// TODO Should start by checking if we can write to the file(s)
		if(rBeep) {
			// Now prepare the list of frames we'll actually use for the beep
			beepFrames = new MediaCtrlFrames;
			beepFrames->clear();
			Prompt *prompt = beepPrompt;
			PromptInstance *promptInstance = new PromptInstance(prompt);
			FILE *file = promptInstance->openFile();
			fseek(file, 0, SEEK_END);
			int filePt = MEDIACTRL_RAW;
			long int size = ftell(file);
			int err = 0, total = 0, bytes = 0;
			unsigned char buffer[320];
			if(prompt->getFilename().find(".gsm") != string::npos) {
				cout << "[IVR] File is a .gsm" << endl;
				bytes = 33;
				filePt = MEDIACTRL_CODEC_GSM;
			} else if(prompt->getFilename().find(".wav") != string::npos) {
				cout << "[IVR] File is a .wav" << endl;
				bytes = 320;
				wavHdr header;
				err = fread(&header, sizeof(header), 1, file);
				size -= 44;	// Don't count header
				filePt = MEDIACTRL_RAW;
			} else {
				cout << "[IVR]     Invalid extension: " << prompt->getFilename() << endl;
				errorString << "Invalid extension for file " << prompt->getName();
				delete promptInstance;
				delete beepFrames;
				beepFrames = NULL;
				return 419;
			}
			MediaCtrlFrame *newframe = NULL;
			rewind(file);
			total = 0;
			while(total < size) {	// Queue all the audio file
				err = fread(buffer, sizeof(unsigned char), bytes, file);
				if(err <= 0)
					break;
				total += err;
				newframe = new MediaCtrlFrame(MEDIACTRL_MEDIA_AUDIO, buffer, err, filePt);	// FIXME
				if(newframe) {
					newframe->setAllocator(IVR);
					if(filePt == MEDIACTRL_RAW)	// Already raw
						beepFrames->push_back(newframe);
					else {	// Decode first
						MediaCtrlFrame *decoded = pkg->callback->decode(newframe);
						if(decoded) {
							beepFrames->push_back(decoded);
						}
					}
				}
			}
			promptInstance->closeFile();
			delete promptInstance;
			delete beepFrames;	// This is just a test to see we can build it
			beepFrames = NULL;
		}
	}

	dlgState = DIALOG_PREPARED;
	cout << "[IVR] iterations=\"" << dec << iterations << "\" duration=\"" << dec << duration << "\"" << endl;
	if(dlgModel & MODEL_PROMPT) {
		cout << "[IVR] <prompt> settings for the dialog:" << endl;
		cout << "[IVR] xml:base=\"" << pBase << "\" bargein=\"" << (pBargein ? "true" : "false") << "\"" << endl;
	}
	if(dlgModel & MODEL_COLLECT) {
		cout << "[IVR] <collect> settings for the dialog:" << endl;
		cout << "[IVR] cleardigitbuffer=\"" << (cCleardigitbuffer ? "true" : "false") << "\" timeout=\"" << dec << cTimeout <<
			"\" interdigittimeout=\"" << dec << cInterdigittimeout << "\" termtimeout=\"" << dec << cTermtimeout <<
			"\" escapekey=\"" << dec << cEscapekey << "\" termchar=\"" << dec << cTermchar <<
			"\" maxdigits=\"" << dec << cMaxdigits << "\"" << endl;
		if(!srgsRules.empty()) {
			int counter = 0;
			SrgsRule *rule = NULL;
			SrgsAlternative *alternative = NULL;
			cout << "[IVR] SRGS grammar settings:" << endl;
			map<string, SrgsRule*>::iterator iter;
			for(iter = srgsRules.begin(); iter != srgsRules.end(); iter++) {
				rule = (SrgsRule*)iter->second;
				if(rule == NULL)
					continue;
				cout << "[IVR] \trule id=" << rule->id << " (" << (rule->privateScope ? "private" : "public") << " scope)" << endl;
				if(!rule->privateScope && (ruleToMatch == ""))
					ruleToMatch = rule->id;
				if(ruleToMatch == rule->id)
					cout << "[IVR] \t\tThis is the rule to match for DTMF" << endl;
				if(rule->alternatives.empty())
					continue;
				rule->alternatives.sort();
				rule->alternatives.unique();
				list<SrgsAlternative*>::iterator iter2;
				counter = 0;
				for(iter2 = rule->alternatives.begin(); iter2 != rule->alternatives.end(); iter2++) {
					counter++;
					alternative = (SrgsAlternative*)(*iter2);
					if(alternative == NULL)
						continue;
					cout << "[IVR] \t\t" << dec << counter << ": ";
					list<string>::iterator iter3;
					for(iter3 = alternative->digits.begin(); iter3 != alternative->digits.end(); iter3++) {
						if(iter3 != alternative->digits.begin())
							cout << " | ";
						cout << (*iter3);
					}
					cout << " (" << dec << alternative->repeatMin << "-" << dec << alternative->repeatMax << ")" << endl;
				}
			}
		}
	}
	if(dlgModel & MODEL_CONTROL) {
		cout << "[IVR] <control> settings for the dialog:" << endl;
		cout << "[IVR] gotostartkey=\"" << dec << vStartkey << "\" gotoendkey=\"" << dec << vEndkey <<
			"\" skipinterval=\"" << dec << vSkipinterval <<
			"\" ffkey=\"" << dec << vFfkey << "\" rwkey=\"" << dec << vRwkey <<
			"\" pauseinterval=\"" << dec << vPauseinterval <<
			"\" resumekey=\"" << dec << vResumekey << "\" pausekey=\"" << dec << vPausekey <<
			"\" volumeinterval=\"" << dec << vVolumeinterval <<
			"\" volupkey=\"" << dec << vVolupkey << "\" voldnkey=\"" << dec << vVoldownkey <<
			"\" speedinterval=\"" << dec << vSpeedinterval <<
			"\" speedupkey=\"" << dec << vSpeedupkey << "\" speeddnkey=\"" << dec << vSpeeddownkey << "\"" << endl;
	}
	if(dlgModel & MODEL_RECORD) {
		cout << "[IVR] <record> settings for the dialog:" << endl;
		cout << "[IVR] timeout=\"" << dec << rTimeout << "\"" <<
			"\" vadinitial=\"" << (rVadinitial ? "true" : "false") << "\" vadfinal=\"" << (rVadfinal ? "true" : "false") <<
			"\" dtmfterm=\"" << (rDtmfterm ? "true" : "false") << "\" beep=\"" << (rBeep ? "true" : "false") <<
			"\" maxtime=\"" << dec << rMaxtime << "\" finalsilence=\"" << dec << rFinalsilence << "\"" <<
			"\" append=\"" << (rAppend ? "true" : "false") << "\"" << endl;
	}
	return 0;
}

void IvrDialog::setupStreams()
{
	// Start by checking the connection itself
	if(connection->getType() == CPC_CONFERENCE) {	// FIXME
		if(dlgModel & MODEL_PROMPT) {
			pAudio = true;
		}
		if(dlgModel & MODEL_RECORD) {
			rAudio = true;
		}
	} else {
		int mediaType = connection->getMediaType();
		if(mediaType == MEDIACTRL_MEDIA_AUDIO) {
			if(dlgModel & MODEL_PROMPT)
				pAudio = true;
			if(dlgModel & MODEL_RECORD)
				rAudio = true;
		} else {	// MEDIACTRL_MEDIA_UNKNOWN
			ControlPackageConnection *sub = pkg->callback->getSubConnection(connection, MEDIACTRL_MEDIA_AUDIO);
			if(sub != NULL) {
				if(dlgModel & MODEL_PROMPT)
					pAudio = true;
				if(dlgModel & MODEL_RECORD)
					rAudio = true;
			}
		}
	}
	if(!streams.empty()) {
		list<IvrStream*>::iterator iter;
		IvrStream *stream = NULL;
		for(iter = streams.begin(); iter != streams.end(); iter++) {
			stream = (*iter);
			if(stream == NULL)
				continue;
			if(dlgModel & MODEL_PROMPT) {
				if(stream->mediaType == MEDIACTRL_MEDIA_AUDIO) {
					if((stream->direction == SENDRECV) || (stream->direction == SENDONLY))
						pAudio = true;
					else
						pAudio = false;
				}
			}
			if(dlgModel & MODEL_RECORD) {
				if(stream->mediaType == MEDIACTRL_MEDIA_AUDIO) {
					if((stream->direction == SENDRECV) || (stream->direction == RECVONLY))
						rAudio = true;
					else
						rAudio = false;
				}
			}
		}
	}
	cout << "[IVR] \t\tAudio: play=" << (pAudio ? "true" : "false") << ", rec=" << (rAudio ? "true" : "false") << endl;
	if(dlgModel & MODEL_RECORD) {
		if(rType == "")
			rType = "audio/x-wav";
		cout << "[IVR] \t\tRecording type set to " << rType << endl;
		if(rDests.empty()) {	// No explicit destination requested, use internal specification
			cout << "[IVR] \t\tNo explicit destination requested, using internal specification" << endl;
			if(rType == "audio/x-wav")
				addRecordingType("", true, "audio/x-wav");	// FIXME
			cout << "[IVR] \t\t\tSaving to " << rDests.front()->getFilename() << endl;
		}
	}
}

void IvrDialog::destroy(bool immediate, int exitstatus)
{
	cout << "[IVR] Destroy: " << dialogId << endl;
	cout << "[IVR] \t\tImmediate: " << (immediate ? "true" : "false") << endl;
	this->immediate = immediate;
	currentIteration = iterations;
	dialogexitStatus = exitstatus;
	if(exitstatus == DIALOGEXIT_CONTERMINATE)
		dialogexitString = "Connection terminated";
	else
		dialogexitString = "Dialog terminated";
	if(immediate) {
		termmode = TERMMODE_STOPPED;
		destroyDialog = true;
	}
	if(running) {
		running = false;
		join();
	}
//	struct timeval tv = {1, 0};
//	while(running)
//		select(0, NULL, NULL, NULL, &tv);
//	cout << "[IVR] \t\tThread not running anymore..." << endl;
//	select(0, NULL, NULL, NULL, &tv);
//	cout << "[IVR] \t\tFinally destroying the dialog" << endl;
//	delete this;
	pkg->endDialog(dialogId);
}

int IvrDialog::attachConnection()
{
	if(confId == "")
		connection = pkg->attach(this, connectionId);
	else
		connection = pkg->attach(this, confId);

	if(connection == NULL)
		return -1;
	return 0;
}

string IvrDialog::getModelString()
{
	stringstream model;
	if(dlgModel == MODEL_PROMPT) {	// Only announcement
		model << "playannouncement";
		return model.str();
	}
	if(dlgModel & MODEL_PROMPT)
		model << "prompt";
	if(dlgModel & MODEL_COLLECT)
		model << "collect";
	else if(dlgModel & MODEL_RECORD)
		model << "record";
	return model.str();
}

int IvrDialog::addFilename(int how, string filename, uint32_t timeout, uint16_t soundLevel, uint32_t clipBegin, uint32_t clipEnd)
{
	if((how != TIMELINE_NORMAL) && (how != TIMELINE_PAR) && (how != TIMELINE_SEQ))
		return -1;
	if((filename.find(".wav") == string::npos) &&
		(filename.find(".mp3") != string::npos) &&
		(filename.find(".ogg") != string::npos))	// FIXME FIXME FIXME
			return 422;	// Unsupported playback format
	if(how == TIMELINE_NORMAL) {	// Create a new TimeLine step, closing the previous one
		currentSlot = 0;
		TimeLine *timeLine = new TimeLine();
		endsync.push_back(ENDSYNC_NONE);
		timeLine->pFilenames.push_back(filename);
		timeLine->pTimeouts.push_back(timeout);
		timeLine->pSoundlevel.push_back(soundLevel);
		timeLine->pClipbegin.push_back(clipBegin);
		timeLine->pClipend.push_back(clipEnd);
		cout << "[IVR] Creating new NORMAL TimeLine: step=" << dec << endsync.size() << ", slot=0 (file=" << filename << ")" << endl;
		audioTimeLine[0].push_back(timeLine);
		int i=0;
		for(i=1; i<TRACKS; i++)
			audioTimeLine[i].push_back(new TimeLine());	// FIXME Are we breaking anything?
	} else if(how == TIMELINE_PAR) {	// Handle a new or updated TimeLine step
		TimeLine *timeLine = NULL;
		if(newTimelineStep) {
			currentSlot = 0;
			endsync.push_back(currentEndSync);
			timeLine = new TimeLine();
			cout << "[IVR] Creating new PARALLEL TimeLine: step=" << dec << endsync.size() << ", slot=0 (file=" << filename << ")" << endl;
			audioTimeLine[0].push_back(timeLine);
			int i=0;
			for(i=1; i<TRACKS; i++)
				audioTimeLine[i].push_back(new TimeLine());	// FIXME Are we breaking anything?
		} else {
			bool ok = false;
			int i=0;
			for(i=0; i<TRACKS; i++) {
				timeLine = audioTimeLine[i].back();
				if(timeLine->pFilenames.empty()) {
					currentSlot = i;
					ok = true;
					break;
				}
			}
			if(!ok) {
				return 435;
			}
			cout << "[IVR] Creating new PARALLEL TimeLine: step=" << dec << endsync.size() << ", slot=" << currentSlot << " (file=" << filename << ")" << endl;
		}
		timeLine->pFilenames.push_back(filename);
		timeLine->pTimeouts.push_back(timeout);
		timeLine->pSoundlevel.push_back(soundLevel);
		timeLine->pClipbegin.push_back(clipBegin);
		timeLine->pClipend.push_back(clipEnd);
	} else if(how == TIMELINE_SEQ) {	// Add the file to the current parallel TimeLine step
		TimeLine *timeLine = NULL;
		if(newTimelineStep) {
			currentSlot = 0;
			endsync.push_back(currentEndSync);
			timeLine = new TimeLine();
			audioTimeLine[currentSlot].push_back(timeLine);
			int i=0;
			for(i=0; i<TRACKS; i++) {
				if(i != currentSlot)
					audioTimeLine[i].push_back(new TimeLine());	// FIXME Are we breaking anything?
			}
			timeLine->pFilenames.push_back(filename);
			timeLine->pTimeouts.push_back(timeout);
			timeLine->pSoundlevel.push_back(soundLevel);
			timeLine->pClipbegin.push_back(clipBegin);
			timeLine->pClipend.push_back(clipEnd);
			cout << "[IVR] Creating new SEQUENTIAL TimeLine: step=" << dec << endsync.size() << ", slot=" << currentSlot << " (file=" << filename << ")" << endl;
			cout << "[IVR] \tNow TimeLine step has " << dec << timeLine->pFilenames.size() << " files" << endl;
		} else {
			if(newSlot) {
				bool ok = false;
				int i=0;
				for(i=0; i<TRACKS; i++) {
					timeLine = audioTimeLine[i].back();
					if(timeLine->pFilenames.empty()) {
						currentSlot = i;
						ok = true;
						break;
					}
				}
				if(!ok) {
					return 435;
				}
				timeLine->pFilenames.push_back(filename);
				timeLine->pTimeouts.push_back(timeout);
				timeLine->pSoundlevel.push_back(soundLevel);
				timeLine->pClipbegin.push_back(clipBegin);
				timeLine->pClipend.push_back(clipEnd);
				cout << "[IVR] Creating new SEQUENTIAL TimeLine: step=" << dec << endsync.size() << ", slot=" << currentSlot << " (file=" << filename << ")" << endl;
				cout << "[IVR] \tNow TimeLine step has " << dec << timeLine->pFilenames.size() << " files" << endl;
			} else {	// Append to current slot
				timeLine = audioTimeLine[currentSlot].back();
				timeLine->pFilenames.push_back(filename);
				timeLine->pTimeouts.push_back(timeout);
				timeLine->pSoundlevel.push_back(soundLevel);
				timeLine->pClipbegin.push_back(clipBegin);
				timeLine->pClipend.push_back(clipEnd);
				cout << "[IVR] Appending new SEQUENTIAL TimeLine: step=" << dec << endsync.size() << ", slot=" << currentSlot << " (file=" << filename << ")" << endl;
				cout << "[IVR] \tNow TimeLine step has " << dec << timeLine->pFilenames.size() << " files" << endl;
			}
		}
	}
	return 0;
}

string IvrDialog::checkDataParameter(int model, string name, string value, int *errorCode)
{
	cout << "[IVR] \tChecking parameter " << name << "=" << value << endl;
	*errorCode = 400;	// We're pessimistic by nature...
	regex re;
	string error = name + "=" + value;
	if(model == MODEL_PROMPT) {
		re.assign("xml:base|bargein", regex_constants::icase);
		if(!regex_match(name.c_str(), re)) {
			*errorCode = 431;
			return name;
		}
		if(name == "xml:base") {
			pBase = value;
		} else if(name == "bargein") {
			bool ok = true;
			pBargein = booleanValue(value, &ok);
			if(!ok)
				return error;
		}
		*errorCode = 200;
		return "";
	} else if(model == MODEL_CONTROL) {
		// FIXME If a duplicate key is involved, 413 error; if a tone is unsopported, 400 error
		re.assign("gotostartkey|gotoendkey|skipinterval|ffkey|rwkey|pauseinterval|pausekey|resumekey|volumeinterval|volupkey|voldnkey|speedinterval|speedupkey|speeddnkey|external");
		if(!regex_match(name.c_str(), re)) {
			*errorCode = 431;
			return name;
		}
		if(name == "gotostartkey") {
			bool ok = true;
			vStartkey = dtmfDigit(value, &ok);
			if(!ok)
				return error;
			if((vStartkey == vEndkey) || (vStartkey == vFfkey) || (vStartkey == vRwkey) || (vStartkey == vPausekey) || (vStartkey == vResumekey) || (vStartkey == vVolupkey) || (vStartkey == vVoldownkey) || (vStartkey == vSpeedupkey) || (vStartkey == vSpeeddownkey)) {
				*errorCode = 413;
				error.append(", duplicate key");
				return error;
			}
			vcrMode = true;		// VCR Mode is enabled: collect and playout are simultaneous
		} else if(name == "gotoendkey") {
			bool ok = true;
			vEndkey = dtmfDigit(value, &ok);
			if(!ok)
				return error;
			if((vEndkey == vStartkey) || (vEndkey == vFfkey) || (vEndkey == vRwkey) || (vEndkey == vPausekey) || (vEndkey == vResumekey) || (vEndkey == vVolupkey) || (vEndkey == vVoldownkey) || (vEndkey == vSpeedupkey) || (vEndkey == vSpeeddownkey)) {
				*errorCode = 413;
				error.append(", duplicate key");
				return error;
			}
			vcrMode = true;		// VCR Mode is enabled: collect and playout are simultaneous
		} else if(name == "skipinterval") {
			bool ok = true;
			vSkipinterval = timeDesignation(value, &ok);
			if(!ok)
				return error;
		} else if(name == "ffkey") {
			bool ok = true;
			vFfkey = dtmfDigit(value, &ok);
			if(!ok)
				return error;
			if((vFfkey == vStartkey) || (vFfkey == vEndkey) || (vFfkey == vRwkey) || (vFfkey == vPausekey) || (vFfkey == vResumekey) || (vFfkey == vVolupkey) || (vFfkey == vVoldownkey) || (vFfkey == vSpeedupkey) || (vFfkey == vSpeeddownkey)) {
				*errorCode = 413;
				error.append(", duplicate key");
				return error;
			}
			vcrMode = true;		// VCR Mode is enabled: collect and playout are simultaneous
		} else if(name == "rwkey") {
			bool ok = true;
			vRwkey = dtmfDigit(value, &ok);
			if(!ok)
				return error;
			if((vRwkey == vStartkey) || (vRwkey == vEndkey) || (vRwkey == vFfkey) || (vRwkey == vPausekey) || (vRwkey == vResumekey) || (vRwkey == vVolupkey) || (vRwkey == vVoldownkey) || (vRwkey == vSpeedupkey) || (vRwkey == vSpeeddownkey)) {
				*errorCode = 413;
				error.append(", duplicate key");
				return error;
			}
			vcrMode = true;		// VCR Mode is enabled: collect and playout are simultaneous
		} else if(name == "pauseinterval") {
			bool ok = true;
			vPauseinterval = timeDesignation(value, &ok);
			if(!ok)
				return error;
		} else if(name == "pausekey") {
			bool ok = true;
			vPausekey = dtmfDigit(value, &ok);
			if(!ok)
				return error;
			if((vPausekey == vStartkey) || (vPausekey == vEndkey) || (vPausekey == vRwkey) || (vPausekey == vFfkey) || (vPausekey == vVolupkey) || (vPausekey == vVoldownkey) || (vPausekey == vSpeedupkey) || (vPausekey == vSpeeddownkey)) {
				*errorCode = 413;
				error.append(", duplicate key");
				return error;
			}
			vcrMode = true;		// VCR Mode is enabled: collect and playout are simultaneous
		} else if(name == "resumekey") {
			bool ok = true;
			vResumekey = dtmfDigit(value, &ok);
			if(!ok)
				return error;
			if((vResumekey == vStartkey) || (vResumekey == vEndkey) || (vResumekey == vRwkey) || (vResumekey == vFfkey) || (vResumekey == vVolupkey) || (vResumekey == vVoldownkey) || (vResumekey == vSpeedupkey) || (vResumekey == vSpeeddownkey)) {
				*errorCode = 413;
				error.append(", duplicate key");
				return error;
			}
			vcrMode = true;		// VCR Mode is enabled: collect and playout are simultaneous
		} else if(name == "volumeinterval") {
			bool ok = true;
			vVolumeinterval = percentValue(value, &ok);
			if(!ok)
				return error;
		} else if(name == "volupkey") {
			bool ok = true;
			vVolupkey = dtmfDigit(value, &ok);
			if(!ok)
				return error;
			if((vVolupkey == vStartkey) || (vVolupkey == vEndkey) || (vVolupkey == vRwkey) || (vVolupkey == vPausekey) || (vVolupkey == vResumekey) || (vVolupkey == vFfkey) || (vVolupkey == vVoldownkey) || (vVolupkey == vSpeedupkey) || (vVolupkey == vSpeeddownkey)) {
				*errorCode = 413;
				error.append(", duplicate key");
				return error;
			}
			vcrMode = true;		// VCR Mode is enabled: collect and playout are simultaneous
		} else if(name == "voldnkey") {
			bool ok = true;
			vVoldownkey = dtmfDigit(value, &ok);
			if(!ok)
				return error;
			if((vVoldownkey == vStartkey) || (vVoldownkey == vEndkey) || (vVoldownkey == vRwkey) || (vVoldownkey == vPausekey) || (vVoldownkey == vResumekey) || (vVoldownkey == vVolupkey) || (vVoldownkey == vFfkey) || (vVoldownkey == vSpeedupkey) || (vVoldownkey == vSpeeddownkey)) {
				*errorCode = 413;
				error.append(", duplicate key");
				return error;
			}
			vcrMode = true;		// VCR Mode is enabled: collect and playout are simultaneous
		} else if(name == "speedinterval") {
			bool ok = true;
			vSpeedinterval = percentValue(value, &ok);
			if(!ok)
				return error;
		} else if(name == "speedupkey") {
			bool ok = true;
			vSpeedupkey = dtmfDigit(value, &ok);
			if(!ok)
				return error;
			if((vSpeedupkey == vStartkey) || (vSpeedupkey == vEndkey) || (vSpeedupkey == vRwkey) || (vSpeedupkey == vPausekey) || (vSpeedupkey == vResumekey) || (vSpeedupkey == vFfkey) || (vSpeedupkey == vVoldownkey) || (vSpeedupkey == vSpeeddownkey)) {
				*errorCode = 413;
				error.append(", duplicate key");
				return error;
			}
			vcrMode = true;		// VCR Mode is enabled: collect and playout are simultaneous
		} else if(name == "speeddnkey") {
			bool ok = true;
			vSpeeddownkey = dtmfDigit(value, &ok);
			if(!ok)
				return error;
			if((vSpeeddownkey == vStartkey) || (vSpeeddownkey == vEndkey) || (vSpeeddownkey == vRwkey) || (vSpeeddownkey == vPausekey) || (vSpeeddownkey == vResumekey) || (vSpeeddownkey == vVolupkey) || (vSpeeddownkey == vFfkey) || (vSpeeddownkey == vSpeedupkey)) {
				*errorCode = 413;
				error.append(", duplicate key");
				return error;
			}
			vcrMode = true;		// VCR Mode is enabled: collect and playout are simultaneous
		} else if(name == "external") {
			return error;
			// TODO Implement external DTMF controls, we trigger an error at the directive at the moment
		}
		*errorCode = 200;
		return "";
	} else if(model == MODEL_COLLECT) {
		re.assign("cleardigitbuffer|timeout|interdigittimeout|termtimeout|escapekey|termchar|maxdigits", regex_constants::icase);
		if(!regex_match(name.c_str(), re)) {
			*errorCode = 431;
			return name;
		}
		if(name == "cleardigitbuffer") {
			bool ok = true;
			cCleardigitbuffer = booleanValue(value, &ok);
			if(!ok)
				return error;
		} else if(name == "timeout") {
			bool ok = true;
			cTimeout = timeDesignation(value, &ok);
			if(!ok)
				return error;
		} else if(name == "interdigittimeout") {
			bool ok = true;
			cInterdigittimeout = timeDesignation(value, &ok);
			if(!ok)
				return error;
		} else if(name == "termtimeout") {
			bool ok = true;
			cTermtimeout = timeDesignation(value, &ok);
			if(!ok)
				return error;
		} else if(name == "escapekey") {
			bool ok = true;
			cEscapekey = dtmfDigit(value, &ok);
			if(!ok)
				return error;
			if(cEscapekey == cTermchar) {
				error.append(", duplicate key (termchar)");
				return error;
			}
		} else if(name == "termchar") {
			bool ok = true;
			cTermchar = dtmfDigit(value, &ok);
			if(!ok)
				return error;
			if(cTermchar == cEscapekey) {
				error.append(", duplicate key (escapekey)");
				return error;
			}
		} else if(name == "maxdigits") {
			bool ok = true;
			cMaxdigits = positiveInteger(value, &ok);
			if(!ok)
				return error;
		}
		*errorCode = 200;
		return "";
	} else if(model == MODEL_RECORD) {
		re.assign("timeout|vadinitial|vadfinal|dtmfterm|maxtime|beep|finalsilence|append", regex_constants::icase);
		if(!regex_match(name.c_str(), re))
			return name;
		if(name == "timeout") {
			bool ok = true;
			rTimeout = timeDesignation(value, &ok);
			if(!ok)
				return error;
		} else if(name == "vadinitial") {
			bool ok = true;
			rVadinitial = booleanValue(value, &ok);
			if(!ok)
				return error;
		} else if(name == "vadfinal") {
			bool ok = true;
			rVadfinal = booleanValue(value, &ok);
			if(!ok)
				return error;
		} else if(name == "dtmfterm") {
			bool ok = true;
			rDtmfterm = booleanValue(value, &ok);
			if(!ok)
				return error;
		} else if(name == "maxtime") {
			bool ok = true;
			rMaxtime = timeDesignation(value, &ok);
			if(!ok)
				return error;
		} else if(name == "beep") {
			bool ok = true;
			rBeep = booleanValue(value, &ok);
			if(!ok)
				return error;
		} else if(name == "finalsilence") {
			bool ok = true;
			rFinalsilence = timeDesignation(value, &ok);
			if(!ok)
				return error;
		} else if(name == "append") {
			bool ok = true;
			rAppend = booleanValue(value, &ok);
			if(!ok)
				return error;
		}
		*errorCode = 200;
		return "";
	}
	*errorCode = 200;
	return "";
}

void IvrDialog::addRecordingType(string dest, bool audio, string type)
{
	// TODO Involve audio/type
	if(type == "audio/x-wav") {
		string filename = "recording-" + dialogId + "-" + random_string(4) + ".wav";
		string path = pkg->webPath + "/" + pkg->recordingsPath + "/" + filename;
		AudioFile *recordingFile = new AudioFile(filename, path, audio);
		recordingFile->setDestination(dest);
		rDests.push_back(recordingFile);
	}
}

void IvrDialog::notifyEvent(string body)
{
	// Use CONTROL for events notification
	stringstream event;
//	event << "<?xml version=\"1.0\"?>";
	event << "<mscivr version=\"1.0\" xmlns=\"urn:ietf:params:xml:ns:msc-ivr\">";
	event << "<event dialogid=\"" << dialogId << "\">";
	event << body << "</event></mscivr>";
	// Send the event
	pkg->callback->control(pkg, sender, event.str());		// FIXME
}

void IvrDialog::playAnnouncements()
{
	playing = false;
	bool firstAudioFrame = true;

	int *sIndex = NULL, aStream[TRACKS];
	AVFormatContext *fctx = NULL, *afctx[TRACKS];
	AVCodecContext *ctx = NULL, *actx[TRACKS];
	AVCodec *codec = NULL, *acodec[TRACKS];
	ReSampleContext *resample[TRACKS];
	AVFrame *incoming = NULL, *packetFrame = NULL;
	AVPacket packet;
	packet.data = NULL;
	int gotFrame = 0, err = 0;
	MediaCtrlFrame *newframe = NULL;

	dlgState = DIALOG_STARTED;	// FIXME
	int track=0, j=0;
	for(track=0; track < TRACKS; track++) {
		if(pAudio && (audioAnnouncements[track] != NULL))
			cout << "[IVR] \t\tSending " << dec << audioAnnouncements[track]->size() << " audio packets (track=" << dec << track << ")..." << endl;
		afctx[track] = NULL;
		actx[track] = NULL;
		acodec[track] = NULL;
		resample[track] = NULL;
		aStream[track] = -1;
	}

	playing = true;
	int vcrEvent = VCR_NONE;
	pVcrEvents.clear();
	AnnouncementFrame *annc = NULL;
	MediaCtrlFrame *frame = NULL;
	list<AnnouncementFrame *>::iterator iter;

	uint32_t skip = 0, pause = 0, i = 0, index = 0;
	uint32_t *remainingSkip = NULL, vRemainingSkip = 0, aRemainingSkip[TRACKS];		// Only needed when seeking goes across different files
	for(track=0; track<TRACKS; track++)
		aRemainingSkip[track] = 0;
	bool paused = false;
	if((vFfkey != MEDIACTRL_DTMF_NONE) || (vRwkey != MEDIACTRL_DTMF_NONE)) {	// FastForward or Rewind enabled
		skip = vSkipinterval/20;	// Number of frames to skip (each frame is 20 ms)
		cout << "[IVR] \t\t\tSkip interval is set to " << dec << vSkipinterval << "ms" << endl;
	}
	if((vPausekey != MEDIACTRL_DTMF_NONE) || (vResumekey != MEDIACTRL_DTMF_NONE)) {	// Pause or Resume enabled
		cout << "[IVR] \t\t\tPause interval is set to " << dec << vPauseinterval << "ms" << endl;
	}

	struct timeval tv;
	uint32_t time = 0;

	uint16_t pVolume = 100;		// The volume of each prompt
	uint16_t aVolume[track];		// The volume as requested via soundLevel

	// Timer to use as a clock FIXME
	int sleepTime = 20000;	// By default, we sleep 20ms, which is what we need for audio
	TimerPort *clockTimer = new TimerPort();
	clockTimer->incTimer(0);	// Start timer
	uint32_t currentTime = clockTimer->getElapsed();	//, startAudioTime = currentTime;
	int64_t startAudioTime = av_gettime(), startPausedTime = 0;
	bool sendAudio = true;
	bool *seekRequested = NULL, *seekForward = NULL, *seekGoto = NULL;	// Needed for VCR
	bool aSeekRequested[TRACKS];
	bool aSeekForward[TRACKS];
	bool aSeekGoto[TRACKS];
	uint32_t currentAudioDuration[TRACKS], *currentDuration = NULL;
	PromptInstance *audioPromptInstance[TRACKS];
	FILE *file[TRACKS];
	list<AnnouncementFrame *>::iterator currentAudioAnnouncement[TRACKS];
	for(track=0; track<TRACKS; track++) {
		file[track] = NULL;
		currentAudioDuration[track] = 0;
		audioPromptInstance[track] = NULL;
		aVolume[track] = 100;
		aSeekRequested[track] = aSeekForward[track] = aSeekGoto[track] = false;
		if(audioAnnouncements[track] != NULL)
			currentAudioAnnouncement[track] = audioAnnouncements[track]->begin();
	}
	list<AnnouncementFrame *>::iterator currentAnnouncement;
	AnnouncementFrames *announcements = NULL;
	Prompt *prompt = NULL;
	PromptInstance *promptInstance = NULL;

	bool audioDone[TRACKS];
	for(track=0; track<TRACKS; track++) {
		if(!pAudio || (audioAnnouncements[track] == NULL) || audioAnnouncements[track]->empty())
			audioDone[track] = true;
		else
			audioDone[track] = false;
	}

	long int mixedBuffer[160];	// Mix of the parallel tracks
	short int outBuffer[160];	// Mix of the parallel tracks (trimmed)
	int8_t *buffer = (int8_t *)MCMALLOC(AVCODEC_MAX_AUDIO_FRAME_SIZE, sizeof(int8_t));
	int8_t *inBuffer = (int8_t *)MCMALLOC(AVCODEC_MAX_AUDIO_FRAME_SIZE, sizeof(int8_t));
	// For resampling (FIFO)
	int size = 0;
	int8_t *resampleBuf = (int8_t*)MCMALLOC(2 * AVCODEC_MAX_AUDIO_FRAME_SIZE, sizeof(int8_t));
	int8_t *fifoBuf = (int8_t*)MCMALLOC(2 * AVCODEC_MAX_AUDIO_FRAME_SIZE, sizeof(int8_t));
	AVFifoBuffer fifo[TRACKS];
	for(track=0; track<TRACKS; track++)
		av_fifo_init(&fifo[track], 2 * AVCODEC_MAX_AUDIO_FRAME_SIZE);

	while(playing) {	// We loop until the announcement is over: VCR controls might delay its end
		if(pAudio) {	// FIXME We use first track (track=0) as reference for all tracks
			for(track=0; track<TRACKS; track++) {
				if((audioAnnouncements[track] == NULL) || audioAnnouncements[track]->empty())
					continue;
				if(!audioDone[track] && (currentAudioAnnouncement[track] == audioAnnouncements[track]->end())) {
					cout << "[IVR] \t\tAudio #" << dec << (track-1) << " is over" << endl;
					audioDone[track] = true;	// The audio announcement is over
				}
			}
		}
		if(audioDone[0] && audioDone[1] && audioDone[2] && audioDone[3]) {	// FIXME
			cout << "[IVR] \t\tPlayback is over" << endl;
			break;	// We're done
		}

		if(duration > 0) {	// There's a maximum duration for the dialog, check if we reached it
			time = timer->getElapsed();
			if((time-startTime) >= duration) {
				cout << "[IVR] \t\tPlayback is over (duration exceeded)" << endl;
				dialogexitStatus = DIALOGEXIT_MAXDURATION;
				dialogexitString = "Maximum dialog duration reached";
				termmode = TERMMODE_STOPPED;
				currentIteration = iterations;
				destroyDialog = true;
				break;
			}
		}
		if(!paused) {
			int step = -1;
			memset(mixedBuffer, 0, 640);
			while(step < (TRACKS+1)) {
				step++;
				if(step == TRACKS)	// TRACKS(4?) Audio
					break;
				announcements = audioAnnouncements[step];
				if(destroyDialog || audioDone[step] || !pAudio || !sendAudio || !announcements || announcements->empty())	// Is it time to send an audio frame?
					continue;
				currentAnnouncement = currentAudioAnnouncement[step];
				promptInstance = audioPromptInstance[step];
				fctx = afctx[step];
				ctx = actx[step];
				sIndex = &aStream[step];
				seekRequested = &aSeekRequested[step];
				seekForward = &aSeekForward[step];
				seekGoto = &aSeekGoto[step];
				remainingSkip = &aRemainingSkip[step];
				currentDuration = &currentAudioDuration[step];
				if(!destroyDialog) {	// Is it ok to send a frame?
					annc = (*currentAnnouncement);
					if((currentAnnouncement != announcements->end()) && (annc != NULL)) {
						aTiming = annc->getDuration();
						if(promptInstance != annc->getPromptInstance()) {	// Prompt has changed
							if(ctx) {
								avcodec_close(ctx);
								ctx = NULL;
								actx[step] = NULL;
							}
							if(fctx) {
								av_close_input_file(fctx);
								fctx = NULL;
								afctx[step] = NULL;
							}
							if(av_fifo_size(&fifo[step]) > 0)
								av_fifo_drain(&fifo[step], av_fifo_size(&fifo[step]));
							if(resample[step] != NULL)
								audio_resample_close(resample[step]);
							resample[step] = NULL;
							promptInstance = annc->getPromptInstance();
							audioPromptInstance[step] = promptInstance;
							if(promptInstance == NULL) {
								prompt = NULL;
							} else {
								prompt = promptInstance->getPrompt();
								if(prompt != NULL) {
									av_open_input_file(&fctx, prompt->getFilename().c_str(), NULL, 0, NULL);
									av_find_stream_info(fctx);
									dump_format(fctx, 0, prompt->getFilename().c_str(), 0);
									av_seek_frame(fctx, -1, 0, AVSEEK_FLAG_BACKWARD);
									// Take the first audio track
									for(i=0; i<fctx->nb_streams; i++) {
										ctx = fctx->streams[i]->codec;
										if(ctx->codec_type == CODEC_TYPE_AUDIO)
											break;
									}
									cout << "Step #" << dec << step << "(audio) --> stream #" << dec << i << endl;
									aStream[step] = i;
									afctx[step] = fctx;
									actx[step] = ctx;
									codec = avcodec_find_decoder(ctx->codec_id);
									acodec[step] = codec;
									resample[step] = audio_resample_init(1, ctx->channels, 8000, ctx->sample_rate);
									if(resample[step] == NULL)
										cout << "[IVR] \t\tError creating resammpler, audio will fail!" << endl;
									avcodec_open(ctx, codec);
									if(annc->getPromptInstance()->getClipBegin() > 0) {
										// Then seek to the I-frame timestamp before our timestamp, and decode until we reach the one we want
										int64_t pts = annc->getTimestamp();
										av_seek_frame(fctx, 0, pts, AVSEEK_FLAG_BACKWARD);
										ctx->hurry_up = 1;
										if(packet.data)
											av_free_packet(&packet);
										do {
											if(av_read_frame(fctx, &packet) < 0) {
												if(packet.data)
													av_free_packet(&packet);
												break;
											}
											if(packet.stream_index != 0)	// FIXME
												continue;
											if(packet.pts >= pts)
												break;
											err = avcodec_decode_audio2(ctx, (int16_t*)inBuffer, &size, packet.data, packet.size);
										} while(1);
										if(packet.data)
											av_free_packet(&packet);
										ctx->hurry_up = 0;
									}
								}
							}
						}
						if(*seekRequested) {
							int64_t pts = 0;
							// First of all, find the timestamp we're interested to
							if(*seekForward) {
								pts = annc->getTimestamp();
								uint32_t currentMs = 1000*(pts*(fctx->streams[*sIndex]->time_base.num))/(fctx->streams[*sIndex]->time_base.den), newMs = currentMs;
								if(*seekGoto) {
									while(1) {
//										cout << dec << currentMs << "/" << newMs << "/" << vSkipinterval << endl;
										currentAnnouncement++;
										if(currentAnnouncement == announcements->end())
											break;
										annc = (*currentAnnouncement);
										if(annc == NULL)
											break;
										*currentDuration = *currentDuration + annc->getDuration();
									}
								} else {
									bool changed = false;
									while((newMs-currentMs) < *remainingSkip) {	// FIXME was vSkipinterval
//										cout << dec << currentMs << "/" << newMs << "/" << vSkipinterval << endl;
										currentAnnouncement++;
										if(currentAnnouncement == announcements->end())
											break;
										annc = (*currentAnnouncement);
										if(annc == NULL)
											break;
										*currentDuration = *currentDuration + annc->getDuration();
										if(promptInstance != annc->getPromptInstance()) {
											*remainingSkip -= (newMs-currentMs);
											changed = true;
											break;
										}
										pts = annc->getTimestamp();
										newMs = 1000*(pts*(fctx->streams[*sIndex]->time_base.num))/(fctx->streams[*sIndex]->time_base.den);
									}
									if(changed)
										continue;	// Seeking proceeds with the next announcement
								}
							} else {
								pts = annc->getTimestamp();
								uint32_t currentMs = 1000*(pts*(fctx->streams[*sIndex]->time_base.num))/(fctx->streams[*sIndex]->time_base.den), newMs = currentMs;
								if(*seekGoto) {
									while(1) {
//										cout << dec << currentMs << "/" << newMs << "/" << vSkipinterval << endl;
										if(currentAnnouncement == announcements->begin())
											break;
										currentAnnouncement--;
										annc = (*currentAnnouncement);
										if(annc == NULL)
											break;
										*currentDuration = *currentDuration - annc->getDuration();
										pts = annc->getTimestamp();
										newMs = 1000*(pts*(fctx->streams[*sIndex]->time_base.num))/(fctx->streams[*sIndex]->time_base.den);
									}
								} else {
									bool changed = false;
									while((currentMs-newMs) < *remainingSkip) {	// FIXME was vSkipInterval
//										cout << dec << currentMs << "/" << newMs << "/" << vSkipinterval << endl;
										if(currentAnnouncement == announcements->begin())
											break;
										currentAnnouncement--;
										annc = (*currentAnnouncement);
										if(annc == NULL)
											break;
										*currentDuration = *currentDuration - annc->getDuration();
										if(promptInstance != annc->getPromptInstance()) {
											*remainingSkip -= (currentMs-newMs);
											changed = true;
											break;
										}
										pts = annc->getTimestamp();
										newMs = (pts*(fctx->streams[*sIndex]->time_base.num))/(fctx->streams[*sIndex]->time_base.den/1000);
									}
									if(changed)
										continue;	// Seeking proceeds with the previous announcement
								}
							}
							currentAudioAnnouncement[step] = currentAnnouncement;
//								cout << "Audio #" << dec << step << ": new duration " << dec << currentAudioDuration[step] << "/" << audioDuration[step] << endl;
							if(currentAudioDuration[step] >= audioDuration[step]) {
								cout << "[IVR] \t\tAudio #" << dec << step << " is over" << endl;
								audioDone[step] = true;
							}
							if(currentAnnouncement == announcements->end()) {
								cout << "[IVR] \t\tAudio #" << dec << step << " is over" << endl;
								audioDone[step] = true;
								continue;
							}
							// Then seek to the frame timestamp before our timestamp, and decode until we reach the one we want
							av_seek_frame(fctx, 0, pts, AVSEEK_FLAG_BACKWARD);
							ctx->hurry_up = 1;
							if(packet.data)
								av_free_packet(&packet);
							do {
								if(av_read_frame(fctx, &packet) < 0) {
									if(packet.data)
										av_free_packet(&packet);
									packet.data = NULL;
									break;
								}
								if(packet.stream_index != 0)	// FIXME
									continue;
								if(packet.pts >= pts)
									break;
								err = avcodec_decode_audio2(ctx, (int16_t*)inBuffer, &size, packet.data, packet.size);
							} while(1);
							if(packet.data)
								av_free_packet(&packet);
							ctx->hurry_up = 0;
						}
						if((annc != NULL) && (currentAnnouncement != announcements->end()) && (promptInstance != NULL) && (prompt != NULL) && (fctx != NULL)) {
							newframe = NULL;
							if(*seekRequested)
								*seekRequested = false;
							if(*seekGoto)
								*seekGoto = false;
							else {	// We don't have a valid packet yet, grab one now
								if(av_fifo_size(&fifo[step]) < 320) {	// Bufferize from the file into the fifo
									int from = 0;
									while(1) {	// Repeat until we get a valid frame
										if(resample[step] == NULL)
											break;
										if(packet.data)
											av_free_packet(&packet);
										if(av_read_frame(fctx, &packet) < 0) {
											if(packet.data)
												av_free_packet(&packet);
											packet.data = NULL;
											cout << "[IVR] \t\tError reading packet... audio #" << dec << step << " is over" << endl;
											audioDone[step] = true;
											break;
										}
										if(packet.stream_index != *sIndex)	// FIXME
											continue;
										currentAnnouncement++;	// FIXME
										*currentDuration = *currentDuration + annc->getDuration();
										size = AVCODEC_MAX_AUDIO_FRAME_SIZE;
										err = avcodec_decode_audio2(ctx, (int16_t*)inBuffer, &size, packet.data, packet.size);
										from = size;
										if((err < 0) || (from < 1))
											break;
										err = audio_resample(resample[step], (int16_t*)resampleBuf, (int16_t*)inBuffer, from/(ctx->channels*2));
										if(err > 0)
											av_fifo_write(&fifo[step], (uint8_t*)resampleBuf, err*2);
										if(av_fifo_size(&fifo[step]) >= 320)
											break;	// Enough buffering...
									}
								}
								if(av_fifo_size(&fifo[step]) >= 320) {
									int16_t curBuffer[160];
									err = av_fifo_read(&fifo[step], (uint8_t*)curBuffer, 320);
									if(err == 0) {
										long int tmpBuffer[160];
										for(j=0; j<160; j++) {
											tmpBuffer[j] = curBuffer[j];
											if(pVolume != 100)	// Change volume (it's not 100%)
												tmpBuffer[j] = tmpBuffer[j]*pVolume/100;
											if(aVolume[step] != 100)	// Change volume (it's not 100%)
												tmpBuffer[j] = tmpBuffer[j]*aVolume[step]/100;
											if(tmpBuffer[j] > SHRT_MAX)
												tmpBuffer[j] = SHRT_MAX;	// TODO Update max/min for subsequent normalization instead?
											else if(tmpBuffer[j] < SHRT_MIN)
												tmpBuffer[j] = SHRT_MIN;
										}
										// Now add the new buffer to the tracks mix
										for(j=0; j<160; j++) {
											mixedBuffer[j] = mixedBuffer[j] + tmpBuffer[j];
											if(mixedBuffer[j] > SHRT_MAX)
												mixedBuffer[j] = SHRT_MAX;	// TODO Update max/min for subsequent normalization instead?
											else if(mixedBuffer[j] < SHRT_MIN)
												mixedBuffer[j] = SHRT_MIN;
										}
									}
								}
							}
						}
						currentAudioAnnouncement[step] = currentAnnouncement;
//							cout << "Audio #" << dec << step << ": new duration " << dec << currentAudioDuration[step] << "/" << audioDuration[step] << endl;
						if(currentAudioDuration[step] >= audioDuration[step]) {
							cout << "[IVR] \t\tAudio #" << dec << step << " is over" << endl;
							audioDone[step] = true;
						}
						if(currentAnnouncement == announcements->end()) {
							cout << "[IVR] \t\tAudio #" << dec << step << " is over" << endl;
							audioDone[step] = true;
							continue;
						}
					}
				}
			}
			// Send prepared Audio
			if(!destroyDialog && pAudio && sendAudio) {
				// Mix all the buffer tracks first
				for(j=0; j<160; j++)
					outBuffer[j] = mixedBuffer[j];
				MediaCtrlFrame *outgoingFrame = new MediaCtrlFrame();
				outgoingFrame->setAllocator(IVR);
				outgoingFrame->setBuffer((uint8_t*)outBuffer, 320);	// FIXME
				if(connection && outgoingFrame) {
					outgoingFrame->setOwner(this);
					if(firstAudioFrame) {
						outgoingFrame->setLocking();
						firstAudioFrame = false;
					}
					if(!destroyDialog) {
						if(connection->getType() == CPC_CONFERENCE)
							outgoingFrame->setTransactionId(tid);
						pkg->callback->sendFrame(connection, outgoingFrame);	// Send the frame
					}
				}
				sendAudio = false;
			}
		}
		if(destroyDialog)
			break;
		if(!pVcrEvents.empty()) {
			while(!pVcrEvents.empty()) {	// VCR events received, enforce them
				vcrEvent = pVcrEvents.front();
				pVcrEvents.pop_front();
				switch(vcrEvent) {
					case VCR_START:
						if(paused) {	// Resume too
							paused = false;
							startPausedTime = 0;
						}
						cout << "[IVR] \t\t\tGoto Start" << endl;
						for(track=0; track<TRACKS; track++) {
							aSeekRequested[track] = true;
							aSeekForward[track] = false;
							aSeekGoto[track] = true;
						}
						break;
					case VCR_END:
						if(paused) {	// Resume too
							paused = false;
							startPausedTime = 0;
						}
						cout << "[IVR] \t\t\tGoto End" << endl;
						for(track=0; track<TRACKS; track++) {
							aSeekRequested[track] = true;
							aSeekForward[track] = true;
							aSeekGoto[track] = true;
						}
						break;
					case VCR_FF:
						if(paused) {	// Resume too
							paused = false;
							startPausedTime = 0;
						}
						cout << "[IVR] \t\t\tFast Forward (" << dec << (skip*20) << "ms)" << endl;
						for(track=0; track<TRACKS; track++) {
							aSeekRequested[track] = true;
							aSeekForward[track] = true;
							aSeekGoto[track] = false;
							aRemainingSkip[track] = vSkipinterval;
						}
						break;
					case VCR_RW:
						if(paused) {	// Resume too
							paused = false;
							startPausedTime = 0;
						}
						cout << "[IVR] \t\t\tRewind (" << dec << (skip*20) << "ms)" << endl;
						for(track=0; track<TRACKS; track++) {
							aSeekRequested[track] = true;
							aSeekForward[track] = false;
							aSeekGoto[track] = false;
							aRemainingSkip[track] = vSkipinterval;
						}
						break;
					case VCR_PAUSE:
						cout << "[IVR] \t\t\tPause" << endl;
						if(!paused) {	// Pause
							paused = true;
							startPausedTime = startAudioTime;
						} else {
							if(vPausekey != vResumekey)	// Already paused
								cout << "[IVR] \t\t\t\tAlready paused, ignoring input" << endl;
							else {	// Pause and resume are the same key
								cout << "[IVR] \t\t\t\tPause and resume are the same key, resuming playout" << endl;
								paused = false;
								startPausedTime = 0;
							}
						}
						break;
					case VCR_RESUME:
						cout << "[IVR] \t\t\tResume" << endl;
						if(paused) {	// Resume
							paused = false;
							startPausedTime = 0;
						} else {
							if(vPausekey != vResumekey)	// Not paused
								cout << "[IVR] \t\t\t\tNot paused, ignoring input" << endl;
							else {	// Pause and resume are the same key
								cout << "[IVR] \t\t\t\tPause and resume are the same key, pausing playout" << endl;
								paused = true;
								startPausedTime = 0;
							}
						}
						break;
					case VCR_VOLUP:
						pVolume += vVolumeinterval;	// TODO Add threshold
						if(pVolume > 500)
							pVolume = 500;
						if(paused) {	// Resume too
							paused = false;
							startPausedTime = 0;
						}
						cout << "[IVR] \t\t\tVolume Up (" << dec << vVolumeinterval << "%), now " << dec << pVolume << "%" << endl;
						break;
					case VCR_VOLDOWN:
						if(pVolume < vVolumeinterval)
							pVolume = 0;
						else
							pVolume -= vVolumeinterval;
						if(paused) {	// Resume too
							paused = false;
							startPausedTime = 0;
						}
						cout << "[IVR] \t\t\tVolume Down (" << dec << vVolumeinterval << "%), now " << dec << pVolume << "%" << endl;
						break;
					case VCR_SPEEDUP:
						if(paused) {	// Resume too
							paused = false;
							startPausedTime = 0;
						}
						cout << "[IVR] \t\t\tSpeed up (" << dec << vSpeedinterval << "%) (ignored)" << endl;	// TODO
						break;
					case VCR_SPEEDDOWN:
						if(paused) {	// Resume too
							paused = false;
							startPausedTime = 0;
						}
						cout << "[IVR] \t\t\tSpeed down (" << dec << vVolumeinterval << "%) (ignored)" << endl;	// TODO
						break;
					default:
						break;
				}
			}
		}
		tv.tv_sec = 0;
		tv.tv_usec = sleepTime;	// FIXME Iterate each specified ms
		select(0, NULL, NULL, NULL, &tv);
		currentTime = clockTimer->getElapsed();
		if(!audioDone[0] || !audioDone[1] || !audioDone[2] || !audioDone[3]) {	// FIXME
			if((av_gettime()-startAudioTime)>=19980) {	// FIXME
				startAudioTime += 20000;
				if(paused) {
					if((av_gettime()-startPausedTime)>=(vPauseinterval*1000)) {	// Reset the counter, and resume the playout
						cout << "[IVR] \t\tPause interval reached (" << dec << vPauseinterval << "ms), resuming the playout" << endl;
						startPausedTime = 0;
						paused = false;
					}
				}
				if(!paused) {	// We might have just unpaused, check again
					i++;	// Go to the next frame
					sendAudio = true;
				}
			}
		}
		i++;	// Go to the next frame
	}
	cout << "[IVR] \t\tPlayback is over, freeing the announcements..." << endl;
	cout << "[IVR] \t\t\tgetting rid of audio..." << endl;
	if(pAudio) {
		frame = new MediaCtrlFrame();
		frame->setAllocator(IVR);
		frame->setOwner(this);
		frame->setUnlocking();
		if(connection) {
			if(connection->getType() == CPC_CONFERENCE)
				frame->setTransactionId(tid);
			pkg->callback->sendFrame(connection, frame);	// Send the fake frame to notify the end of the announcement
		}
	}
	if(resampleBuf != NULL)
		MCMFREE(resampleBuf);
	resampleBuf = NULL;
	if(buffer != NULL)
		MCMFREE(buffer);
	buffer = NULL;
	if(inBuffer != NULL)
		MCMFREE(inBuffer);
	inBuffer = NULL;
	for(track=0; track<TRACKS; track++) {
		cout << "[IVR] \t\t\ttrack=" << dec << track << endl;
		if(audioPromptInstance[track])
			audioPromptInstance[track]->closeFile();
		if(actx[track]) {
			avcodec_close(actx[track]);
			actx[track] = NULL;
		}
		if(afctx[track]) {
			av_close_input_file(afctx[track]);
			afctx[track] = NULL;
		}
		if(resample[track] != NULL)
			audio_resample_close(resample[track]);
		resample[track] = NULL;
		av_fifo_free(&fifo[track]);
	}
	clockTimer->endTimer();
	delete clockTimer;
	clockTimer = NULL;

	if(!playing)
		cout << "[IVR] \t\tFrames sent (interrupted)" << endl;
	else
		cout << "[IVR] \t\tFrames sent" << endl;

	playing = false;
}

void IvrDialog::playBeep()
{
	if(beepFrames == NULL) {
		// Now prepare the list of frames we'll actually use for the beep
		beepFrames = new MediaCtrlFrames;
		beepFrames->clear();
		Prompt *prompt = beepPrompt;
		PromptInstance *promptInstance = new PromptInstance(prompt);
		FILE *file = promptInstance->openFile();
		fseek(file, 0, SEEK_END);
		int filePt = MEDIACTRL_RAW;
		long int size = ftell(file);
		int err = 0, total = 0, bytes = 0;
		unsigned char buffer[320];
		if(prompt->getFilename().find(".gsm") != string::npos) {
			cout << "[IVR] File is a .gsm" << endl;
			bytes = 33;
			filePt = MEDIACTRL_CODEC_GSM;
		} else if(prompt->getFilename().find(".wav") != string::npos) {
			cout << "[IVR] File is a .wav" << endl;
			bytes = 320;
			wavHdr header;
			err = fread(&header, sizeof(header), 1, file);
			size -= 44;	// Don't count header
			filePt = MEDIACTRL_RAW;
		} else {
			cout << "[IVR]     Invalid extension: " << prompt->getFilename() << endl;
			errorString << "Invalid extension for file " << prompt->getName();
			delete promptInstance;
			delete beepFrames;
			beepFrames = NULL;
			return;
		}
		MediaCtrlFrame *newframe = NULL;
		rewind(file);
		total = 0;
		while(total < size) {	// Queue all the audio file
			err = fread(buffer, sizeof(unsigned char), bytes, file);
			if(err <= 0)
				break;
			total += err;
			newframe = new MediaCtrlFrame(MEDIACTRL_MEDIA_AUDIO, buffer, err, filePt);	// FIXME
			if(newframe) {
				newframe->setAllocator(IVR);
				if(filePt == MEDIACTRL_RAW)	// Already raw
					beepFrames->push_back(newframe);
				else {	// Decode first
					MediaCtrlFrame *decoded = pkg->callback->decode(newframe);
					if(decoded) {
						beepFrames->push_back(decoded);
					}
				}
			}
		}
		promptInstance->closeFile();
		delete promptInstance;
	}
	struct timeval now, before;
	gettimeofday(&before, NULL);
	now.tv_sec = before.tv_sec;
	now.tv_usec = before.tv_usec;
	time_t passed, d_s, d_us;
	playing = true;
	MediaCtrlFrame *frame = NULL;
	list<MediaCtrlFrame*>::iterator iter;
	int i = 0;
	while(playing) {
		if(i == beepFrames->size())
			break;	// The beep is over
		if(destroyDialog)
			break;
		iter = beepFrames->begin();
		int j=0;
		for(j = 0; j < i; j++) {
			iter++;	// FIXME
			if(iter == beepFrames->end())
				break;
		}
		frame = (*iter);
		if(connection) {
			if(connection->getType() == CPC_CONFERENCE)
				frame->setTransactionId(tid);
			pkg->callback->sendFrame(connection, frame);	// Send the frame
		}
		while(1) {
			gettimeofday(&now, NULL);
			d_s = now.tv_sec - before.tv_sec;
			d_us = now.tv_usec - before.tv_usec;
			if(d_us < 0) {
				d_us += 1000000;
				--d_s;
			}
			passed = d_s*1000000 + d_us;
			if(passed >= 18500)
				break;
			usleep(1000);
		}
		// Update the reference time
		before.tv_usec += 20000;
		if(before.tv_usec > 1000000) {
			before.tv_sec++;
			before.tv_usec -= 1000000;
		}
		i++;	// Go to the next frame
	}
	delete beepFrames;
	beepFrames = NULL;
	playing = false;
}

void IvrDialog::run()
{
	dlgState = DIALOG_STARTING;
	running = true;
	bool replied = false;
	cout << "[IVR] IvrDialog thread starting: " << dialogId << endl;
	setupStreams();		// Needed to adjust settings related to prompt/record/etc according to what the <streams> told us
	// Check if we prepared some unneeded frames
	if(!pAudio) {	// Remove audio announcements
		int i=0;
		for(i=0; i<TRACKS; i++) {
			if(audioAnnouncements[i] != NULL) {
				cout << "[IVR] Freeing audio announcements (track=" << dec << i << ")" << endl;
				while(!audioAnnouncements[i]->empty()) {
					AnnouncementFrame *annc = audioAnnouncements[i]->front();
					audioAnnouncements[i]->pop_front();
					if(annc != NULL)
						delete annc;
				}
				delete audioAnnouncements[i];
				audioAnnouncements[i] = NULL;
			}
			if(!audioTimeLine[i].empty()) {
				while(!audioTimeLine[i].empty()) {
					TimeLine *audioTL = audioTimeLine[i].front();
					audioTimeLine[i].pop_front();
					if(audioTL != NULL)
						delete audioTL;
				}
			}
			if(!audioPrompts.empty()) {
				while(!audioPrompts.empty()) {
					PromptInstance *instance = audioPrompts.front();
					audioPrompts.pop_front();
					if(instance != NULL)
						delete instance;
				}
			}
		}
	}
	currentIteration = 1;
	int tempTone = -1;
	// Start a timer, needed for duration
	startTime = 0;
	uint32_t time = 0;
	timer = new TimerPort();
	timer->incTimer(0);	// Start timer
	startTime = timer->getElapsed();

	string path;	// Path to filename used for recording
	uint32_t recordTime = 0;

	while(currentIteration <= iterations) {
		done = false;
		destroyDialog = false;
		replied = false;
		if(vcrMode)
			cCleardigitbuffer = true;
		if((dlgModel & MODEL_COLLECT) && cCleardigitbuffer)	// We clear the buffer before anything is started, in order to get bargein tones as valid DTMF input for <collect>
			pkg->clearDtmfBuffer(connection);
		if(dlgModel & MODEL_PROMPT) {
			vcrInfo = "";
			termmode = TERMMODE_UNKNOWN;
			bool play = true;
			if((dlgModel & MODEL_COLLECT) && pBargein && !vcrMode && !cCleardigitbuffer) {
				tempTone = pkg->getNextDtmfBuffer(connection);
				if(tempTone != -1) {	// Since there's a previously bufferized tone, we never start the announcement (+bargin, -cleardigitbuffer)
					termmode = TERMMODE_BARGEIN;
					play = false;
				}
			}
			if(play) {
				cout << "[IVR] \t\tprompt::Iteration: " << dec << currentIteration << "/" << dec << iterations << endl;
				TimerPort *promptTimer = new TimerPort();
				promptTimer->incTimer(0);
				uint32_t promptStart = promptTimer->getElapsed();
				if(pAudio)
					playAnnouncements();				// Play the announcement
				time = promptTimer->getElapsed() - promptStart;		// How much time did it take?
				delete promptTimer;
				promptTimer = NULL;
			}
			if(termmode == TERMMODE_UNKNOWN)
				termmode = TERMMODE_COMPLETED;
			stringstream durationString;
			durationString << dec << time;
			string termmodeString = "";
			switch(termmode) {
				case TERMMODE_BARGEIN:
					termmodeString = "bargein";
					break;
				case TERMMODE_COMPLETED:
					termmodeString = "completed";
					break;
				case TERMMODE_STOPPED:
					termmodeString = "stopped";
					break;
				default:
					termmodeString = "??";
					break;
			}
			if(currentIteration == iterations) {	// TODO Take into account other stopping reasons...
				if(destroyDialog && immediate)
					cout << "[IVR] dialogterminate with immediate=true, skipping <promptinfo>/<controlinfo> report..." << endl;
				else {
					stringstream info;
					info << "<promptinfo duration=\"" << durationString.str() << "\" termmode=\"" << termmodeString << "\"/>";
					dialogInfo << info.str();
					if(vcrMode) {
						if(vcrInfo == "")
							dialogInfo << "<controlinfo/>";
						else
							dialogInfo << "<controlinfo>" << vcrInfo << "</controlinfo>";
					}
				}
			}
		}
		if(dlgModel & MODEL_COLLECT) {
			cout << "[IVR] \t\tcollect::Iteration: " << dec << currentIteration << "/" << dec << iterations << endl;
collectstartpoint:
//			if(vcrMode)
//				cCleardigitbuffer = true;
			if(restart) {	// The escapekey was pressed and the flow was redirected here
				restart = false;
				cCleardigitbuffer = true;	// We need to clean the previous input anyway
				pkg->clearDtmfBuffer(connection);
			}
			termmode = TERMMODE_UNKNOWN;
			done = false;
			tones.clear();
			dlgState = DIALOG_STARTED;	// FIXME
//			if(cCleardigitbuffer)
//				pkg->clearDtmfBuffer(connection);
//				cout << "[IVR] \t\tRemoving bufferized DTMF tones (cleardigitbuffer=true)..." << endl;
//			else {		// Get bufferized tones first
//				cout << "[IVR] \t\tTrying to get bufferized DTMF tones (cleardigitbuffer=false)..." << endl;
				mTones.enter();
				if(tempTone != -1) {	// First check if anything interrupted playback before it even started...
					if(tempTone == cTermchar)
						done = true;
					else if(tempTone == cEscapekey)
						restart = true;
					else {
						tones.push_back(tempTone);
						tempTone = -1;
						if(tones.size() >= cMaxdigits)
							done = true;
					}
				}
				if(!done && !restart) {
					int tone = -1;
					while(1) {
						tone = pkg->getNextDtmfBuffer(connection);
						if(tone < 0) {
							cout << "[IVR] \t\t\tok, done" << endl;
							break;
						}
						cout << "[IVR] \t\t\tGot bufferized DTMF tone: " << dec << tone << endl;
						if(tone == cTermchar) {
							done = true;
							break;
						}
						if(tone == cEscapekey) {
							restart = true;
							break;
						}
						tones.push_back(tone);
						if(tones.size() >= cMaxdigits) {
							done = true;
							break;
						}
					}
				}
				mTones.leave();
//			}
			if(restart)
				goto collectstartpoint;
			cout << "[IVR] \t\tCollecting DTMF tones..." << endl;
			// Collect stuff
			collecting = true;
			cMatch = true;	// We're optimist (SRGS grammar would change this to false, in case)
			if(!done && cMatch) {	// We need more digits
				uint16_t tonesSize = tones.size();
				if(tonesSize >= cMaxdigits)
					done = true;
				else {
					bool timeoutReached = false, interdigittimeoutReached = false;
					uint32_t time = 0, dtmfStartTime = 0, latestDtmfTime = 0;
					TimerPort *collectTimer = new TimerPort();
					collectTimer->incTimer(0);	// Start timer
					dtmfStartTime = latestDtmfTime = collectTimer->getElapsed();
					tonesSize = 0;
					struct timeval tv;
					while(!done) {
						if(duration > 0) {	// There's a maximum duration for the dialog, check if we reached it
							time = timer->getElapsed();
							if((time-startTime) >= duration) {
								cout << "[IVR] \t\tCollect is over (duration exceeded)" << endl;
								dialogexitStatus = DIALOGEXIT_MAXDURATION;
								dialogexitString = "Maximum dialog duration reached";
								termmode = TERMMODE_STOPPED;
								currentIteration = iterations;
								destroyDialog = true;
								break;
							}
						}
						if(destroyDialog || done || timeoutReached || interdigittimeoutReached || restart)
							break;
						time = collectTimer->getElapsed();
						while(tonesSize < tones.size()) {	// Received new digit
							// tonesSize = tones.size();
							tonesSize++;
							latestDtmfTime = time;
							if(tonesSize >= cMaxdigits)
								done = true;
						}
						if((dtmfStartTime == latestDtmfTime) && ((time-dtmfStartTime) >= cTimeout))
							timeoutReached = true;
						if((dtmfStartTime != latestDtmfTime) && ((time-latestDtmfTime) >= cInterdigittimeout))
							interdigittimeoutReached = true;
						if(done || timeoutReached || interdigittimeoutReached || restart)
							break;
						tv.tv_sec = 0;
						tv.tv_usec = 20000;
						select(0, NULL, NULL, NULL, &tv);
					}
					delete collectTimer;
					collectTimer = NULL;
					if(restart)
						goto collectstartpoint;
					if(tones.size() > 0)
						done = true;
				}
			}
			collecting = false;
			if(!done && !destroyDialog) {	// Error
				if(currentIteration == iterations)	// TODO Take into account other stopping reasons...
					dialogInfo << "<collectinfo termmode=\"noinput\"/>";
			} else {	// Success	(TODO Implement nomatch and stopped)
				DtmfTones::iterator iter;
				stringstream dtmfBuffer;
				int tone = -1;
				if(tones.empty())
					dtmfString = "";
				else {
					for(iter = tones.begin(); iter != tones.end(); iter++) {
						tone = (*iter);
						dtmfBuffer << dtmfCode(tone);
					}
					dtmfString = dtmfBuffer.str();
				}
				mTones.leave();
				cout << "[IVR] IvrDialog dtmfString: " << (dtmfString == "" ? "(none)" : dtmfString) << endl;
				if(notifyCollect && done && cMatch && (dtmfString != ""))
					notifyEvent("<dtmfnotify matchmode=\"collect\" dtmf=\"" + dtmfString + "\" timestamp=\"" + dtmfTimestamp() + "\"/>");
				if(currentIteration == iterations) {	// TODO Take into account other stopping reasons...
					if(destroyDialog && immediate)
						cout << "[IVR] dialogterminate with immediate=true, skipping <collectinfo> report..." << endl;
					else {
						if(done) {
							if(cMatch)
								dialogInfo << "<collectinfo dtmf=\"" << dtmfString << "\" termmode=\"match\"/>";
							else
								dialogInfo << "<collectinfo dtmf=\"" << dtmfString << "\" termmode=\"nomatch\"/>";
						} else {
							if(!cMatch)
								termmode = TERMMODE_NOMATCH;
							dialogInfo << "<collectinfo ";
							if(dtmfString != "")
								dialogInfo << "dtmf=\"" << dtmfString << "\" ";
							if(termmode == TERMMODE_NOINPUT)
								dialogInfo << "termmode=\"noinput\"/>";
							else if(termmode == TERMMODE_NOMATCH)
								dialogInfo << "termmode=\"nomatch\"/>";
							else if(termmode == TERMMODE_STOPPED)
								dialogInfo << "termmode=\"stopped\"/>";
						}
					}
				}
			}
		} else if(dlgModel & MODEL_RECORD) {
			cout << "[IVR] \t\trecord::Iteration: " << dec << currentIteration << "/" << dec << iterations << endl;
			bool newFile = false;
			if(!rAppend)
				newFile = true;	// Always create a new file at each iteration, we're not allowed to append
			else if(currentIteration == 1)
				newFile = true;	// We still don't have a valid file for recording
			if(newFile) {
				list<RecordingFile*>::iterator iter;
				RecordingFile *recordingFile = NULL;
				for(iter = rDests.begin(); iter != rDests.end(); iter++) {
					recordingFile = (*iter);
					if(recordingFile == NULL)
						continue;
					termmode = TERMMODE_UNKNOWN;
					if(recordingFile->getType() == "audio/x-wav") {
						AudioFile *audioFile = (AudioFile *)recordingFile;
						if(rAppend && (iterations > 1)) {	// We first need to destroy the previous instance
							AudioFile *newAudioFile = new AudioFile(audioFile->getFilename(), audioFile->getPath(), audioFile->getRecordAudio());
							delete audioFile;
							audioFile = newAudioFile;
							(*iter) = audioFile;
						}
						if(!audioFile->prepare())
							cout << "[IVR]    Error preparing the audio file!! -- " << audioFile->getFilename() << endl;
					}
				}
			}
			if(rBeep) {	// Play beep if needed
				cout << "[IVR] Playing the beep" << endl;	// FIXME
				playBeep();
//				while(playing);
				playing = false;	// FIXME
			}
			dlgState = DIALOG_STARTED;	// FIXME
			rInputreceived = false;
			recording = true;
			list<RecordingFile*>::iterator iterR;
			RecordingFile *recordingFile = NULL;
			for(iterR = rDests.begin(); iterR != rDests.end(); iterR++) {
				recordingFile = (*iterR);
				if(recordingFile == NULL)
					continue;
			}					
			cout << "[IVR] Starting the recording" << endl;	// FIXME
			// TODO dest, which is ignored currently
			uint32_t time = 0, recordStartTime = 0, timeoutStartTime = 0;
			TimerPort *recordTimer = NULL, *timeoutTimer = NULL;
			if(!rVadinitial || !rAudio)	// FIXME
				rInputreceived = true;		// Don't check any timeout, there's no VAD to start recording
			else {
				timeoutTimer = new TimerPort();
				timeoutTimer->incTimer(0);	// Start timeout timer
				timeoutStartTime = timeoutTimer->getElapsed();
			}
			if(!destroyDialog && !done) {
				struct timeval tv;
				while(!destroyDialog && !done) {
					if(duration > 0) {	// There's a maximum duration for the dialog, check if we reached it
						time = timer->getElapsed();
						if((time-startTime) >= duration) {
							cout << "[IVR] \t\tRecording is over (duration exceeded)" << endl;
							dialogexitStatus = DIALOGEXIT_MAXDURATION;
							dialogexitString = "Maximum dialog duration reached";
							termmode = TERMMODE_STOPPED;
							currentIteration = iterations;
							done = true;
							break;
						}
					}
					if(rInputreceived && rVadfinal && (rSilencestarttime > 0)) {
						// Check if we have enough silence to stop the recording
						if((rSilencetimer->getElapsed()-rSilencestarttime) >= rFinalsilence) {
							cout << "[IVR] finalsilence detected (exceeded " << dec << rFinalsilence << "ms)" << endl;
							termmode = TERMMODE_FINALSILENCE;
							done = true;
						}
					}
					if(destroyDialog || done)
						break;
					if(!rInputreceived && rVadinitial) {	// Check the noinput timeout
						time = timeoutTimer->getElapsed();
						if((time-timeoutStartTime) >= rTimeout) {
							cout << "[IVR] No voice activity detected within the timeout (" << dec << rTimeout << "ms), stopping the recording" << endl;
							termmode = TERMMODE_NOINPUT;
							done = true;
						}
					}
					if(rInputreceived && (recordTimer == NULL)) {	// Actually start the recording
						cout << "[IVR] Voice activity detected, actually starting the recording" << endl;
						recordTimer = new TimerPort();
						recordTimer->incTimer(0);	// Start timer
						recordStartTime = recordTimer->getElapsed();
						if(rVadfinal) {
							if(rSilencetimer == NULL) {
								rSilencetimer = new TimerPort();
								rSilencetimer->incTimer(0);
								rSilencestarttime = 0;
							}
						}
					}
					if(recordTimer != NULL) {
						time = recordTimer->getElapsed();
						if((time-recordStartTime) >= rMaxtime) {
							termmode = TERMMODE_MAXTIME;
							done = true;
						}
					}
					if(done)
						break;
					tv.tv_sec = 0;
					tv.tv_usec = 100000;
					select(0, NULL, NULL, NULL, &tv);
				}
			}
			if(rAppend)
				recordTime += time;
			else
				recordTime = time;
			delete recordTimer;
			recordTimer = NULL;
			if(rVadfinal && (rSilencetimer != NULL)) {
				delete rSilencetimer;
				rSilencetimer = NULL;
			}
			recording = false;
			cout << "[IVR] Done recording!" << endl;	// FIXME
			long int size = 0;
			if(currentIteration == iterations) {
				if(destroyDialog && immediate)
					cout << "[IVR] dialogterminate with immediate=true, skipping <recordinfo> report..." << endl;
				else {
					if(!done) {	// Error
						if(destroyDialog)
							dialogInfo << "<recordinfo termmode=\"stopped\"/>";
						else
							dialogInfo << "<recordinfo termmode=\"noinput\"/>";
					} else {	// Success
						if(termmode == TERMMODE_NOINPUT) {
							dialogInfo << "<recordinfo termmode=\"noinput\"/>";
						} else {
							cout << "[IVR] IvrDialog recording: " << endl;
							replied = true;
							list<RecordingFile*>::iterator iter;
							RecordingFile *recordingFile = NULL;
							stringstream mediaInfo;
							for(iter = rDests.begin(); iter != rDests.end(); iter++) {
								recordingFile = (*iter);
								if(recordingFile == NULL)
									continue;	// FIXME Should be error?
								string filename = recordingFile->getFilename();
								string path = recordingFile->getPath();
								string dest = recordingFile->getDestination();
								string type = recordingFile->getType();
								delete recordingFile;
								struct stat results;
								stat(path.c_str(), &results);
								size = results.st_size;
								string url = "";
								// Build the implementation-specific URL first
								if(pkg->webPort == "80")	// Default port for HTTP, don't write it
									url = "http://" + pkg->webAddress +
												"/" + pkg->recordingsPath + "/" +
												filename;	// FIXME
								else	// Non-default port for the webserver, add it to the URL
									url = "http://" + pkg->webAddress + ":" +
												pkg->webPort +
												"/" + pkg->recordingsPath + "/" +
												filename;	// FIXME
								string recordedUrl = url;
								if(dest != "") {	// Upload the file to destination
									int errorCode = uploadFile(dest, path);	// FIXME
									if(errorCode != 0) {	// An error occurred while uploading the file
										recordedUrl = url;	// FIXME
										termmode = TERMMODE_STOPPED;
									} else {
										recordedUrl = dest;
									}								
								}
								mediaInfo << "<mediainfo loc=\"" << recordedUrl << "\" " << "type=\"" << type
										<< "\" size=\"" << dec << size << "\"/>";
							}
							dialogInfo << "<recordinfo duration=\"" << dec << recordTime << "\" termmode=\"";
							string termmodeString = "";
							if(termmode == TERMMODE_MAXTIME)
								dialogInfo << "maxtime";
							else if(termmode == TERMMODE_DTMF)
								dialogInfo << "dtmf";
							else if(termmode == TERMMODE_FINALSILENCE)
								dialogInfo << "finalsilence";
							else if(termmode == TERMMODE_STOPPED)
								dialogInfo << "stopped";
							else
								dialogInfo << "??";
							// TODO Add a different <mediainfo> for every recorded file
							dialogInfo << "\"";
							if(mediaInfo.str() == "")
								dialogInfo << "/>";
							else
								dialogInfo << ">" << mediaInfo.str() << "</recordinfo>";
						}
					}
				}
			}
		}
		currentIteration++;
	}

	if(connection != NULL) {
		pkg->detach(this, connection);
		connection = NULL;
	}

	if(dialogexitStatus == -1) {	// Nothing strange happened
		dialogexitStatus = DIALOGEXIT_SUCCESS;
		dialogexitString = "Dialog successfully completed";
	}

	stringstream exit;
	if(destroyDialog && immediate)
		exit << "<dialogexit status=\"" << dec << dialogexitStatus << "\" reason=\"" << dialogexitString << "\"/>";
	else
		exit << "<dialogexit status=\"" << dec << dialogexitStatus << "\" reason=\"" << dialogexitString << "\">" << dialogInfo.str() << "</dialogexit>";
	notifyEvent(exit.str());

	if(destroyDialog && !immediate && !replied) {
		cout << "[IVR] Unknown error, missing event!!!" << endl;
	}

	cout << "[IVR] IvrDialog thread leaving: " << dialogId << endl;
	running = false;
	destroy(true, DIALOGEXIT_SUCCESS);
}

void IvrDialog::incomingFrame(ControlPackageConnection *connection, ControlPackageConnection *subConnection, MediaCtrlFrame *frame)
{
	if(dlgState != DIALOG_STARTED) {	// We're not ready yet, drop the frame...
		return;
	}

	if(!(dlgModel & MODEL_RECORD)) {
		return;
	}
	// Record only when needed by the template
	if(destroyDialog || done || playing || collecting || !recording) {	// Ignore additional frames
		return;
	}
	if((this->connection == connection) || (this->connection == subConnection)) {
		if((connection->getMediaType() != MEDIACTRL_MEDIA_UNKNOWN) && (connection->getMediaType() != frame->getMediaType())) {
			return;
		}
		if(rAudio && (rVadinitial || rVadfinal)) {
			if(frame->getMediaType() == MEDIACTRL_MEDIA_AUDIO) {
				if(isSilence(frame)) {	// FIXME
					if(rInputreceived && rVadfinal && (rSilencestarttime == 0) && (rSilencetimer != NULL)) {
//						cout << "[IVR] Detected what may be the beginning of a long silence..." << endl;
						rSilencestarttime = rSilencetimer->getElapsed();
					}
					// FIXME Should we remove the frame as well?
					//		PRO the draft says that finalsilence does not belong to the recording
					//		CON intermittent silence needs to belong to it instead!
//					return;
				} else {	// We received the first valid input: remove the timout timer, start the recording timer, and set the first input as received
					rSilencestarttime = 0;
					rInputreceived = true;
				}
			// TODO What were the following lines there for?
//			} else {	// The recording hasn't started yet, we're waiting for voice activity
//				return;
			}
		}
		list<RecordingFile*>::iterator iter;
		for(iter = rDests.begin(); iter != rDests.end(); iter++) {
			RecordingFile *recordingFile = (*iter);
			if(recordingFile == NULL)
				continue;
			if(rAudio && (frame->getMediaType() == MEDIACTRL_MEDIA_AUDIO)) {
				// Check format of the frame, to see if/how we need to decode (we save a slinear audio/wav)
				MediaCtrlFrame *decoded = frame;
				if(frame->getFormat() != MEDIACTRL_RAW) {
					decoded = pkg->callback->decode(frame);
				}
				if(decoded) {
					recordingFile->writeFrame(decoded);
				}
			}
		}
	}
}

void IvrDialog::incomingDtmf(ControlPackageConnection *connection, ControlPackageConnection *subConnection, int type)
{
	if(dlgState != DIALOG_STARTED)		// We're not ready yet, drop the tone...
		return;

	if(done)	// Ignore additional tones
		return;
	if((this->connection == connection) || (this->connection == subConnection)) {
		cout << "[IVR] IvrDialog->incomingDtmf (" << dec << type << ") from " << connection->getConnectionId() << endl;
		if(playing && (dlgModel & MODEL_CONTROL)) {
			// VCR Controls
			bool found = false;
			if(type == vStartkey) {
				pVcrEvents.push_back(VCR_START);
				cout << "[IVR] \t\tQueueing Goto Start VCR control" << endl;
				found = true;
			} else if(type == vEndkey) {
				pVcrEvents.push_back(VCR_END);
				cout << "[IVR] \t\tQueueing Goto End VCR control" << endl;
				found = true;
			} else if(type == vFfkey) {
				pVcrEvents.push_back(VCR_FF);
				cout << "[IVR] \t\tQueueing Fast Forward VCR control" << endl;
				found = true;
			} else if(type == vRwkey) {
				pVcrEvents.push_back(VCR_RW);
				cout << "[IVR] \t\tQueueing Rewind VCR control" << endl;
				found = true;
			} else if(type == vPausekey) {
				pVcrEvents.push_back(VCR_PAUSE);
				cout << "[IVR] \t\tQueueing Pause VCR control" << endl;
				found = true;
			} else if(type == vResumekey) {
				pVcrEvents.push_back(VCR_RESUME);
				cout << "[IVR] \t\tQueueing Resume VCR control" << endl;
				found = true;
			} else if(type == vVolupkey) {
				pVcrEvents.push_back(VCR_VOLUP);
				cout << "[IVR] \t\tQueueing Volume Up VCR control" << endl;
				found = true;
			} else if(type == vVoldownkey) {
				pVcrEvents.push_back(VCR_VOLDOWN);
				cout << "[IVR] \t\tQueueing Volume Down VCR control" << endl;
				found = true;
			} else if(type == vSpeedupkey) {
				pVcrEvents.push_back(VCR_SPEEDUP);
				cout << "[IVR] \t\tQueueing Speed Up VCR control" << endl;
				found = true;
			} else if(type == vSpeeddownkey) {
				pVcrEvents.push_back(VCR_SPEEDDOWN);
				cout << "[IVR] \t\tQueueing Speed Down VCR control" << endl;
				found = true;
			} else if(pBargein) {	// Check bargein
				// Interrupt playback
				termmode = TERMMODE_BARGEIN;
				playing = false;
			}
#if 0
				// Bufferize dtmf (for <collectinfo> only)
				mTones.enter();
				cout << "[IVR] \t\tPushing back tone: " << dec << type << endl;
				tones.push_back(type);
				pkg->clearDtmfBuffer(connection);
				mTones.leave();
#endif
			if(notifyAll || notifyVcr) {
				stringstream info;
				info << "dtmf=\"" << dtmfCode(type) << "\" timestamp=\"" << dtmfTimestamp() << "\"";
				if(found && notifyVcr) {	// Build a controlmatch and, if needed, trigger a dtmfnotify
					vcrInfo.append("<controlmatch " + info.str() + "/>");
					notifyEvent("<dtmfnotify matchmode=\"control\" " + info.str() + "/>");
				}
				if(notifyAll)
					notifyEvent("<dtmfnotify matchmode=\"all\" " + info.str() + "/>");
			}
		} else if(playing && pBargein) {
			if(notifyAll) {		// We have a dtmfsub subscription that wants all digits to be notified
				stringstream info;
				info << "dtmf=\"" << dtmfCode(type) << "\" timestamp=\"" << dtmfTimestamp() << "\"";
				notifyEvent("<dtmfnotify matchmode=\"all\" " + info.str() + "/>");
			}
			// Interrupt playback
			termmode = TERMMODE_BARGEIN;
			playing = false;
		} else if(collecting && (dlgModel & MODEL_COLLECT)) {
			if(notifyAll) {		// We have a dtmfsub subscription that wants all digits to be notified
				stringstream info;
				info << "dtmf=\"" << dtmfCode(type) << "\" timestamp=\"" << dtmfTimestamp() << "\"";
				notifyEvent("<dtmfnotify matchmode=\"all\" " + info.str() + "/>");
			}
			if(type == cEscapekey)
				restart = true;
			else if(type == cTermchar)
				done = true;
			if(done || restart)	// Ignore additional tones
				return;
			// Bufferize dtmf
			mTones.enter();
			cout << "[IVR] \t\tPushing back tone: " << dec << type << endl;
			tones.push_back(type);
			pkg->clearDtmfBuffer(connection);
			mTones.leave();
		} else if(recording && (dlgModel & MODEL_RECORD)) {
			if(notifyAll) {		// We have a dtmfsub subscription that wants all digits to be notified
				stringstream info;
				info << "dtmf=\"" << dtmfCode(type) << "\" timestamp=\"" << dtmfTimestamp() << "\"";
				notifyEvent("<dtmfnotify matchmode=\"all\" " + info.str() + "/>");
			}
			if(rDtmfterm) {
				// Stop recording
				termmode = TERMMODE_DTMF;
				done = true;
			}
		} else {	// Nothing is happening, but we received a DTMF digit anyway...
			if(notifyAll) {		// We have a dtmfsub subscription that wants all digits to be notified
				stringstream info;
				info << "dtmf=\"" << dtmfCode(type) << "\" timestamp=\"" << dtmfTimestamp() << "\"";
				notifyEvent("<dtmfnotify matchmode=\"all\" " + info.str() + "/>");
			}
		}
	}
}

void IvrDialog::connectionLocked(ControlPackageConnection *connection, ControlPackageConnection *subConnection)
{
	if((this->connection == connection) || (this->connection == subConnection))
		cout << "[IVR] Connection locked: " << subConnection->getConnectionId() << endl;
}

void IvrDialog::connectionUnlocked(ControlPackageConnection *connection, ControlPackageConnection *subConnection)
{
	if((this->connection == connection) || (this->connection == subConnection))
		cout << "[IVR] Connection unlocked: " << subConnection->getConnectionId() << endl;
}

void IvrDialog::connectionClosing(ControlPackageConnection *connection, ControlPackageConnection *subConnection)
{
	destroy(true, DIALOGEXIT_CONTERMINATE);	// FIXME Should actually check if ALL the media went down
	this->connection = NULL;
}


// Class Methods
IvrPackage::IvrPackage() {
	MCMINIT();
	cout << "[IVR] Registering IvrPackage()" << endl;
	name = "msc-ivr";
	version = "1.0";
	desc = "Media Server Control - Interactive Voice Response - version 1.0";
	mimeType = "application/msc-ivr+xml";
	curl_global_init(CURL_GLOBAL_ALL);
	messages.clear();
	endedMessages.clear();
	dialogs.clear();
	connections.clear();
	endedDialogs.clear();

	alive = false;
	filesCacheM.enter();
	filesCache.clear();
	filesCacheM.leave();
}

IvrPackage::~IvrPackage() {
	cout << "[IVR] Removing IvrPackage()" << endl;
	if(!dialogs.empty()) {
		map<string, IvrDialog*>::iterator iter;
		for(iter = dialogs.begin(); iter != dialogs.end(); iter++){
			if(iter->second != NULL)
				iter->second->destroy(true, DIALOGEXIT_CONTERMINATE);	// FIXME
		}
	}
	if(alive) {
		alive = false;
		join();
	}
	filesCacheM.enter();
	if(!filesCache.empty()) {
		map<string, Prompt*>::iterator iter;
		Prompt *prompt = NULL;
		while(!filesCache.empty()) {
			iter = filesCache.begin();
			prompt = iter->second;
			if(prompt != NULL)
				delete prompt;
			filesCache.erase(iter);
		}
	}
	filesCacheM.leave();
	curl_global_cleanup();	// FIXME
}

void IvrPackage::setCollector(void *frameCollector)
{
	setCommonCollector(frameCollector);
	cout << "[IVR] Frame Collector " << (getCollector() ? "OK" : "NOT OK :(") << endl;
}

bool IvrPackage::setup()
{
	cout << "[IVR] Initializing ffmpeg related stuff" << endl;
	if(ffmpeg_initialized == false) {
		ffmpeg_initialized = true;
	    avcodec_register_all();
		av_register_all();
	}
	// TODO Fail if anything goes wrong
	// Get the configuration values
	webAddress = callback->getPackageConfValue(this, "webserver", "address");
	if(webAddress == "")
		webAddress = "127.0.0.1";
	string tmp = callback->getPackageConfValue(this, "webserver", "port");
	if((tmp == "") || (atoi(tmp.c_str()) < 1))
		tmp = "8001";
	webPort = tmp;
	webPath = callback->getPackageConfValue(this, "webserver", "local");
	if(webPath == "")
		webPath = "/var/www/html";
	webServerPath = webPath;
	DIR *dir = opendir(webPath.c_str());
	if(!dir)
		cout << "[IVR] *** Couldn't access the webserver local path!!! ***" << endl;
	else
		closedir(dir);
	promptsPath = callback->getPackageConfValue(this, "prompts");
	if(promptsPath == "")
		promptsPath = "prompts";
	tmp = webPath + "/" + promptsPath;
	dir = opendir(tmp.c_str());
	if(!dir)
		cout << "[IVR] *** Couldn't access the webserver 'prompts' local path!!! ***" << endl;
	else
		closedir(dir);
	recordingsPath = callback->getPackageConfValue(this, "recordings");
	if(recordingsPath == "")
		recordingsPath = "recordings";
	tmp = webPath + "/" + recordingsPath;
	dir = opendir(tmp.c_str());
	if(!dir)
		cout << "[IVR] *** Couldn't access the webserver 'recordings' local path!!! ***" << endl;
	else {
		closedir(dir);
		tmp += "/.mediactrl";
		FILE *file = fopen(tmp.c_str(), "wb");
		if(!file)
			cout << "[IVR] *** The webserver 'recordings' local path is not writable!!! ***" << endl;
		else
			fclose(file);
	}
	tmpPath = callback->getPackageConfValue(this, "tmp");
	if(tmpPath == "")
		tmpPath = "/tmp/mediactrl";
	dir = opendir(tmpPath.c_str());
	if(!dir) {
		cout << "[IVR] *** Couldn't access the 'tmp' local path!!! ***" << endl;
		cout << "[IVR] \t\tI will try to create it now..." << endl;
		if(mkdir(tmpPath.c_str(), 0777) < 0)	// FIXME
			cout << "[IVR] \t\t\t*** Failed to create *** (prompts will fail)" << endl;
		else
			cout << "[IVR] \t\t\tSuccessfully created: " << tmpPath << endl;
	} else {
		cout << "[IVR] Cleaning 'tmp' path from old files" << endl;
		char fullPath[255];
		struct dirent *tmpFile = NULL;
		while((tmpFile = readdir(dir))) {
			if(!strcmp(tmpFile->d_name, ".") || !strcmp(tmpFile->d_name, ".."))
				continue;
			memset(fullPath, 0, 255);
			sprintf(fullPath, "%s/%s", tmpPath.c_str(), tmpFile->d_name);
			cout << "[IVR] \tRemoving " << tmpFile->d_name << endl;
			if(remove(fullPath) < 0)
				cout << "[IVR] \t\tFailed! (" << errno << ")" << endl;
		}
		closedir(dir);
	}
	tmp = tmpPath + "/.mediactrl";
	FILE *file = fopen(tmp.c_str(), "wb");
	if(!file)
		cout << "[IVR] *** The 'tmp' path is not writable!!! ***" << endl;
	else
		fclose(file);

	// Create a default Prompt instance for the beep
	cout << "[IVR] Creating default Prompt for the BEEP..." << endl;
	string filename = "file://" + webPath + "/prompts/beep.wav";	// FIXME
	beepPrompt = new Prompt(filename, tmpPath);
	if(beepPrompt->startTransfer(30000) != 0)
		cout << "[IVR]     Couldn't open/get file " << filename.c_str() << endl;
	cout << "[IVR]     Opened/Got prompt file " << filename.c_str() << endl;
	// Cache the file path (unnecessary, probably)
	cout << "[IVR]        Caching the tmp file: " << beepPrompt->getFilename() << endl;
	filesCacheM.enter();
	filesCache[filename] = beepPrompt;
	filesCacheM.leave();

	start();
	return true;
}

ControlPackageConnection *IvrPackage::attach(IvrDialog *dlg, string connectionId) {
	// FIXME
	ControlPackageConnection *connection = callback->getConnection(this, connectionId);
	if(connection == NULL)
		return NULL;
	else {
		connection->addPackage(this);
		connections[connectionId] = dlg;
		return connection;
	}
}

void IvrPackage::detach(IvrDialog *dlg, ControlPackageConnection *connection) {
	if(connection == NULL)
		return;
	cout << "[IVR] Detaching connection " << connection->getConnectionId() << " from dialog " << dlg->getDialogId() << endl;
//	connections[connection->getConnectionId()] = NULL;
	connections.erase(connection->getConnectionId());
	// FIXME Only do this when no more dialogs need this
	connection->removePackage(this);
	if(connection->getType() == CPC_CONNECTION)
		callback->dropConnection(this, connection);	// FIXME
}

string IvrPackage::getInfo()
{
	stringstream info;
	info << "Running: " << alive << endl;
	info << "Transactions:" << endl;
	if(messages.empty())
		info << "\tnone" << endl;
	else {
		list<IvrMessage *>::iterator iter;
		IvrMessage *msg = NULL;
		for(iter = messages.begin(); iter != messages.end(); iter++) {
			msg = (*iter);
			if(msg == NULL)
				continue;
			info << "\t" << msg->tid << endl;
			info << "\t\t" << msg->request << endl;
		}
	}
	info << "Dialogs:" << endl;
	if(dialogs.empty())
		info << "\tnone" << endl;
	else {
		map<string, IvrDialog *>::iterator iter;
		IvrDialog *dialog = NULL;
		for(iter = dialogs.begin(); iter != dialogs.end(); iter++) {
			dialog = iter->second;
			if(dialog == NULL)
				continue;
			info << "\t" << iter->first << endl;
		}
	}
	info << "Connections:" << endl;
	if(connections.empty())
		info << "\tnone" << endl;
	else {
		map<string, IvrDialog *>::iterator iter;
		for(iter = connections.begin(); iter != connections.end(); iter++) {
			info << "\t" << iter->first << endl;
			if(iter->second != NULL)
				info << "\t\tattached to dialog " << iter->second->getDialogId() << endl;
		}
	}
	return info.str();
}

void IvrPackage::control(void *sender, string tid, string blob) {
	cout << "[IVR] \tIvrPackage() received CONTROL message (tid=" << tid << ")" << endl;
	IvrMessage *msg = new IvrMessage();
	msg->pkg = this;
	msg->tid = tid;
	msg->blob = blob;
	msg->sender = sender;
	if(sender == NULL)
		cout << "[IVR] \t\tsender is NULL?? expect problems..." << endl;
	messages.push_back(msg);
}

void IvrPackage::incomingFrame(ControlPackageConnection *connection, ControlPackageConnection *subConnection, MediaCtrlFrame *frame)
{
	IvrDialog *dlg = connections[connection->getConnectionId()];
	if(dlg == NULL) {
		connections.erase(connection->getConnectionId());
		dlg = connections[subConnection->getConnectionId()];
	}
	if(dlg == NULL) {
		connections.erase(subConnection->getConnectionId());
		connection->removePackage(this);
		subConnection->removePackage(this);
	} else	// Send the frame to the interested dialog
		dlg->incomingFrame(connection, subConnection, frame);
}

void IvrPackage::incomingDtmf(ControlPackageConnection *connection, ControlPackageConnection *subConnection, int type)
{
	cout << "[IVR] IvrPackage->incomingDtmf (" << dec << type << ") from " << subConnection->getConnectionId() << endl;
	IvrDialog *dlg = connections[connection->getConnectionId()];
	if(dlg == NULL) {
		connections.erase(connection->getConnectionId());
		dlg = connections[subConnection->getConnectionId()];
	}
	if(dlg == NULL) {
		connections.erase(subConnection->getConnectionId());
		connection->removePackage(this);
		subConnection->removePackage(this);
	} else		// Send the tone to the interested dialog
		dlg->incomingDtmf(connection, subConnection, type);
}

void IvrPackage::connectionLocked(ControlPackageConnection *connection, ControlPackageConnection *subConnection)
{
	IvrDialog *dlg = connections[connection->getConnectionId()];
	if(dlg == NULL) {
		connections.erase(connection->getConnectionId());
		dlg = connections[subConnection->getConnectionId()];
	}
	if(dlg == NULL) {
		connections.erase(subConnection->getConnectionId());
		connection->removePackage(this);
		subConnection->removePackage(this);
	} else
		dlg->connectionLocked(connection, subConnection);

	return;
}

void IvrPackage::connectionUnlocked(ControlPackageConnection *connection, ControlPackageConnection *subConnection)
{
	IvrDialog *dlg = connections[connection->getConnectionId()];
	if(dlg == NULL) {
		connections.erase(connection->getConnectionId());
		dlg = connections[subConnection->getConnectionId()];
	}
	if(dlg == NULL) {
		connections.erase(subConnection->getConnectionId());
		connection->removePackage(this);
		subConnection->removePackage(this);
	} else
		dlg->connectionUnlocked(connection, subConnection);

	return;
}

void IvrPackage::connectionClosing(ControlPackageConnection *connection, ControlPackageConnection *subConnection)
{
	cout << "[IVR] Closed connection " << connection->getConnectionId() << endl;
	IvrDialog *dlg = connections[connection->getConnectionId()];
	if(dlg) {
		detach(dlg, connection);
		connections.erase(connection->getConnectionId());
		dlg->connectionClosing(connection, subConnection);
	} else {
		connections.erase(connection->getConnectionId());
		dlg = connections[subConnection->getConnectionId()];
		connections.erase(subConnection->getConnectionId());
		if(dlg) {
			dlg->connectionClosing(connection, subConnection);
			detach(dlg, subConnection);
		}
	}

	if(connection != NULL)
		connection->removePackage(this);
	if((connection != subConnection) && (subConnection != NULL))
		subConnection->removePackage(this);

	return;
}

void IvrPackage::sendFrame(ControlPackageConnection *connection, MediaCtrlFrame *frame)
{
	// TODO Really needed?
}

void IvrPackage::clearDtmfBuffer(ControlPackageConnection *connection)
{
	callback->clearDtmfBuffer(connection);
}

int IvrPackage::getNextDtmfBuffer(ControlPackageConnection *connection)
{
	return callback->getNextDtmfBuffer(connection);
}

void IvrPackage::run() {
	alive = true;
	cout << "[IVR] Joining IvrPackage->thread()" << endl;

	bool waiting = false;
	while(alive) {
		mEnded.enter();		// Clear zombie dialogs and messages first
		if(!endedDialogs.empty()) {
			while(!endedDialogs.empty()) {
				IvrDialog* dlg = dialogs[endedDialogs.front()];
				dialogs.erase(endedDialogs.front());
				if(dlg != NULL)
					delete dlg;
				endedDialogs.pop_front();
			}
		}
		if(!endedMessages.empty()) {
			while(!endedMessages.empty()) {
				IvrMessage *msg = endedMessages.front();
				endedMessages.pop_front();
				if(msg != NULL)
					delete msg;
			}
		}
		mEnded.leave();
		if(waiting) {
			struct timeval tv = {0, 50000};
			select(0, NULL, NULL, NULL, &tv);
		}
		if(messages.empty()) {
			waiting = true;
			continue;
		}
		waiting = false;
		IvrMessage *msg = messages.front();
		messages.pop_front();
		cout << "[IVR] \tHandling CONTROL message (tid=" << msg->tid << ")" << endl;
		handleControl(msg);
		cout << "[IVR] IvrPackage->thread() back to sleep..." << endl;
	}
	cout << "[IVR] Leaving IvrPackage->thread()" << endl;
}

void IvrPackage::endDialog(string dialogId)
{
	mEnded.enter();
	endedDialogs.push_back(dialogId);
	mEnded.leave();
}

void IvrPackage::handleControl(IvrMessage *msg)
{
	cout << "[IVR] \tIvrPackage->control (tid=" << msg->tid << ")" << endl;
	if(msg->blob == "") {
		cout << "[IVR]     Bodyless CONTROL" << endl;
		// CONTROL without body?
		msg->error(400, "No XML body");
		delete msg;
		return;
	}
	// First scan the XML blob for errors...
	msg->scanonly = true;
	XML_Parser parser = XML_ParserCreate(NULL);
	XML_SetUserData(parser, msg);
	XML_SetElementHandler(parser, startElement, endElement);
	XML_SetCharacterDataHandler(parser, valueElement);
	if (XML_Parse(parser, msg->blob.c_str(), msg->blob.length(), 1) == XML_STATUS_ERROR) {
		cout << "[IVR]     Error parsing IvrPackage CONTROL body: '"
			<< XML_ErrorString(XML_GetErrorCode(parser))
			<< "' at " << XML_GetCurrentLineNumber(parser) << ":"
			<< XML_GetCurrentColumnNumber(parser) << endl;
		cout << "[IVR]     Broken body was:" << endl << msg->blob << "(" << dec << msg->blob.length() << ")" << endl;
		msg->error(400, "Invalid XML body");
		XML_ParserFree(parser);
		delete msg;
		return;
	}
	XML_ParserFree(parser);
	// ... and then actually parse and handle it
	msg->start();
}

void IvrPackage::deleteMessage(IvrMessage *msg)
{
	mEnded.enter();
	endedMessages.push_back(msg);
	mEnded.leave();
}


// eXpat callbacks handle the parsing of CONTROL bodies
static void XMLCALL startElement(void *msg, const char *name, const char **atts)
{
	IvrMessage *message = (IvrMessage *)msg;
	if(message->scanonly)
		return;
	if(message->stop)
		return;
	if(message->level == 0) {
		// FIXME do some initialization here? it's the root
	}
	message->level++;
	message->childs.push_back(name);
	cout << "[IVR] Parsing level " << dec << message->level << " (" << name << ")" << endl;
	if(message->level == 1) {
		if(message->childs.back() != "mscivr") {	// mscivr, root of all requests/responses/notifications
			message->error(400, name);
			return;
		}
		if(!atts) {	// version was not specified
			message->error(400, "version");
			return;
		}
		string version = "", nameSpace = "";
		int i = 0;
		while(atts[i]) {	// We don't care about other attributes
			if(!atts[i])
				break;
			if(!strcmp(atts[i], "version"))
				version = atts[i+1];
			else if(!strcmp(atts[i], "xmlns"))
				nameSpace = atts[i+1];
			else {
				message->error(431, atts[i]);
				return;
			}
			i += 2;
		}
		if(version == "") {
			message->error(400, "version");
			return;
		}
		if(version != "1.0") {
			message->error(400, "version is not 1.0");
			return;
		}
		if((nameSpace == "") || (nameSpace != "urn:ietf:params:xml:ns:msc-ivr")) {
			message->error(431, "Invalid mscivr namespace");
			return;
		}
	} else if(message->level == 2) {
		// Handle allowed elements at level 2 according to level 1
		message->request = name;
		if(message->childs.back() == "dialogprepare") {
			message->newdialog = true;
			string dialogId = "", src = "", type = "", fetchtimeout = "";
			int i = 0;
			while(atts[i]) {
				if(!atts[i])
					break;
				if(!strcmp(atts[i], "src"))
					src = atts[i+1];
				else if(!strcmp(atts[i], "type"))
					type = atts[i+1];
				else if(!strcmp(atts[i], "fetchtimeout"))
					fetchtimeout = atts[i+1];
				else if(!strcmp(atts[i], "dialogid"))
					dialogId = atts[i+1];
				else {
					message->error(431, atts[i]);
					return;
				}
				i += 2;
			}
			if((type != "") && (checkType(type)) < 0) {
				message->error(421, type);
				return;
			}
			// Get the XML blob referenced by the URL in 'src', if specified
			if(src != "") {
				message->externalContent = true;
				bool ok;
				if(fetchtimeout == "")
					fetchtimeout = "30s";
				uint32_t timeout = timeDesignation(fetchtimeout, &ok);
				if(!ok) {
					message->error(400, "fetchtimeout");
					return;
				}
				// TODO We first have to check if the URI is supported (error 420 otherwise)
				long int code = 0;
				message->content = getExternalReference(src, timeout, &code);
				if(message->content == "") {
					stringstream errorCode;
					errorCode << "Error " << dec << code << " for " << src;
					message->error(409, errorCode.str());
					return;
				}
			}
			if(message->pkg->dialogs.find(dialogId) != message->pkg->dialogs.end()) {
				message->error(405, dialogId);
				return;
			} else {
				message->dialog = new IvrDialog(message->pkg, message->sender, dialogId);
				message->dialog->setTransactionId(message->tid);
				dialogId = message->dialog->getDialogId();
			}
		} else if(message->childs.back() == "dialogstart") {
			message->newdialog = true;	// unless...
			int existing = -1;	// 0=no, 1=yes, -1=check <dialog>
			int cType = -1;
			string src = "", type = "", dialogId = "", Id = "", fetchtimeout = "";
			int i = 0;
			while(atts[i]) {	// We don't care about other attributes
				if(!atts[i])
					break;
				if(!strcmp(atts[i], "src")) {
					if(existing == 1) {
						message->error(400, "can't have both src AND prepareddialogid");
						return;
					}
					existing = 0;
					src = atts[i+1];
				} else if(!strcmp(atts[i], "type")) {
					type = atts[i+1];
				} else if(!strcmp(atts[i], "fetchtimeout")) {
					fetchtimeout = atts[i+1];
				} else if(!strcmp(atts[i], "dialogid")) {
					if(existing == 1) {
						message->error(400, "can't have both dialogid AND prepareddialogid");
						return;
					}
					existing = 0;
					dialogId = atts[i+1];
				} else if(!strcmp(atts[i], "prepareddialogid")) {
					if(src != "") {
						message->error(400, "can't have both src AND prepareddialogid");
						return;
					} else if(existing == 0) {
						message->error(400, "can't have both dialogid AND prepareddialogid");
						return;
					}
					existing = 1;
					dialogId = atts[i+1];
				} else if(!strcmp(atts[i], "connectionid")) {
					if(Id != "") {
						message->error(400, "can't have both conferenceid AND connectionid");
						return;
					}
					cType = CONNECTION;
					Id = atts[i+1];
				} else if(!strcmp(atts[i], "conferenceid")) {
					if(Id != "") {
						message->error(400, "can't have both conferenceid AND connectionid");
						return;
					}
					cType = CONFERENCE;
					Id = atts[i+1];
				} else {
					message->error(431, atts[i]);
					return;
				}
				i += 2;
			}
			if(existing <= 0) {
				message->newdialog = true;
				if((type != "") && (checkType(type)) < 0) {
					message->error(421, type);
					return;
				}
				// Get the XML blob referenced by the URL in 'src', if specified
				if(src != "") {
					bool ok;
					if(fetchtimeout == "")
						fetchtimeout = "30s";
					uint32_t timeout = timeDesignation(fetchtimeout, &ok);
					if(!ok) {
						message->error(400, "fetchtimeout");
						return;
					}
					// TODO We first have to check if the URI is supported (error 420 otherwise)
					long int code = 0;
					message->content = getExternalReference(src, timeout, &code);
					if(message->content == "") {
						stringstream errorCode;
						errorCode << "Error " << dec << code << " for " << src;
						message->error(409, errorCode.str());
						return;
					}
				}
				if(message->pkg->dialogs.find(dialogId) != message->pkg->dialogs.end()) {
					message->error(405, dialogId);
					return;
				} else {
					message->dialog = new IvrDialog(message->pkg, message->sender, dialogId);
					message->dialog->setTransactionId(message->tid);
					dialogId = message->dialog->getDialogId();
					if(cType == CONNECTION)
						message->dialog->setConnectionId(Id);
					else
						message->dialog->setConfId(Id);
				}
			} else if(existing == 1) {
				message->newdialog = false;
				if(message->pkg->dialogs.find(dialogId) == message->pkg->dialogs.end()) {
					message->error(406, dialogId);
					return;
				} else {
					message->dialog = message->pkg->dialogs[dialogId];
					if(!message->dialog->checkSender(message->sender)) {	// Unauthorized: not the original requester
						message->pkg->callback->report(message->pkg, message->sender, message->tid, 403, 0);			// FIXME
						message->stop = true;
						return;
					}
					if(cType == CONNECTION)
						message->dialog->setConnectionId(Id);
					else
						message->dialog->setConfId(Id);
					message->dialog->setTransactionId(message->tid);
				}
			}
			message->conntype = cType;
			message->connId = Id;
			if(Id == "") {
				message->error(400, "missing a connection/conference identifier");

			}
		} else if(message->childs.back() == "dialogterminate") {
			message->newdialog = false;
			if(!atts) {	// dialogid was not specified
				message->error(400, "dialogid");
				return;
			} else {	// dialogid was specified, remove it
				string dialogId = "";
				bool immediate = false;
				int i = 0;
				while(atts[i]) {	// We don't care about other attributes
					if(!atts[i])
						break;
					if(!strcmp(atts[i], "dialogid"))
						dialogId = atts[i+1];
					else if(!strcmp(atts[i], "immediate")) {
						bool ok;
						immediate = booleanValue(atts[i+1], &ok);
						if(!ok) {
							message->error(400, "immediate");
							return;
						}
					} else {
						message->error(431, atts[i]);
						return;
					}
					i += 2;
				}
				if(dialogId == "") {
					message->error(400, "dialogid");
					return;
				} else if(message->pkg->dialogs.find(dialogId) == message->pkg->dialogs.end()) {
					message->error(406, dialogId);
					return;
				} else {
					// Remove dialog later
					message->dialog = message->pkg->dialogs[dialogId];
					message->immediate = immediate;
					if(!message->dialog->checkSender(message->sender)) {	// Unauthorized: not the original requester
						message->pkg->callback->report(message->pkg, message->sender, message->tid, 403, 0);			// FIXME
						message->stop = true;
						return;
					}
				}
			}
		} else if(message->childs.back() == "audit") {
			string dialogId = "";
			bool capabilities = true, dialogs = true;
			if(atts) {
				int i = 0;
				while(atts[i]) {	// We don't care about other attributes
					if(!atts[i])
						break;
					if(!strcmp(atts[i], "dialogid"))
						dialogId = atts[i+1];
					else if(!strcmp(atts[i], "capabilities")) {
						bool ok;
						capabilities = booleanValue(atts[i+1], &ok);
						if(!ok) {
							message->error(400, "capabilities");
							return;
						}
					} else if(!strcmp(atts[i], "dialogs")) {
						bool ok;
						dialogs = booleanValue(atts[i+1], &ok);
						if(!ok) {
							message->error(400, "dialogs");
							return;
						}
					} else {
						message->error(431, atts[i]);
						return;
					}
					i += 2;
				}
				if((dialogId != "") && (message->pkg->dialogs.find(dialogId) == message->pkg->dialogs.end())) {
					message->error(406, dialogId);
					return;
				}
			}
			message->auditCapabilities = capabilities;
			message->auditDialogs = dialogs;
			message->auditDialog = dialogId;
		} else {
			message->error(431, name);
			return;
		}
	} else if(message->level == 3) {
		// Handle allowed elements at level 3 according to level 2
		if(message->childs.back() == "dialog") {
			if(message->externalContent) {
				message->error(400, "can't have both src AND inline stuff");
				return;
			}
			if(message->request == "dialogterminate") {
				message->error(400, "dialogterminate doesn't support 'dialog'");	// FIXME Which error is better?
				return;
			} else if(message->request == "dialogstart") {
				if(!message->newdialog) {
					message->error(400, "can't have both 'dialog' AND 'prepareddialogid'");	// FIXME Which error is better?
					return;
				}
			}
			message->digging = true;
			// Save the whole subelement ant its attributes and children: we'll parse it later
			stringstream content;
			if(!atts)	// No attributes
				content << "<dialog>";
			else {
				int i=0;
				content << "<dialog";
				while(atts[i]) {	// Write down all attributes
					if(!atts[i])
						break;
					content << " " << atts[i] << "=\"" << atts[i+1] << "\"";
					i += 2;
				}
				content << ">";
			}
			message->content.append(content.str());
		} else if(message->childs.back() == "params") {
			if(message->request != "dialogstart") {
				message->error(400, "only dialogstart supports 'params'");	// FIXME Which error is better?
				return;
			}
			if(atts) {
				message->error(431, name);	// params has no attributes
				return;
			}
		} else if(message->childs.back() == "stream") {
			if(message->request != "dialogstart") {
				message->error(400, "only dialogstart supports 'stream'");	// FIXME Which error is better?
				return;
			}
			if(!atts) {	// media was not specified
				message->error(400, "media");
				return;
			}
			IvrStream *stream = new IvrStream();
			int i = 0;
			bool mediafound = false;
			while(atts[i]) {
				if(!atts[i])
					break;
				if(!strcmp(atts[i], "media")) {
					mediafound = true;
					stream->media = atts[i+1];
				} else if(!strcmp(atts[i], "label")) {
					stream->label = atts[i+1];
				} else if(!strcmp(atts[i], "direction")) {
					string direction = atts[i+1];
					int streamDirection = parseDirection(direction);
					if(streamDirection < 0) {
						delete stream;
						string error;
						error = "Unsupported direction '" + direction + "'";
						message->error(411, error);
						return;
					}
					stream->direction = streamDirection;
				} else {
					delete stream;
					message->error(431, atts[i]);
					return;
				}
				i += 2;
			}
			if(!mediafound) {	// media was not specified
				delete stream;
				message->error(411, "media");
				return;
			}
			if(stream->media == "audio")
				stream->mediaType = MEDIACTRL_MEDIA_AUDIO;
			else {
				delete stream;
				message->error(411, "Unsupported media");
				return;
			}
			message->streams.push_back(stream);
		} else if(message->childs.back() == "subscribe") {
			if(message->request != "dialogstart") {
				message->error(400, "only dialogstart supports 'subscribe'");	// FIXME Which error is better?
				return;
			}
			if(atts) {
				int i=0;
				while(atts[i]) {	// Write down all attributes
					if(!atts[i])
						break;
					message->error(400, atts[i]);	// subscribe has no attributes
					return;
				}
			}
		} else {
			message->error(431, name);
			return;
		}
		return;
	} else if(message->level == 4) {
		// Handle allowed elements at level 4 according to level 3
		message->childs.pop_back();
		string owner = message->childs.back();
		message->childs.push_back(name);
		if(owner == "dialog") {
			// Save the whole subelement ant its attributes and children: we'll parse it later
			message->digging = true;
			stringstream content;
			if(!atts)	// No attributes
				content << "<" << name << ">";
			else {
				int i=0;
				content << "<" << name;
				while(atts[i]) {	// Write down all attributes
					if(!atts[i])
						break;
					content << " " << atts[i] << "=\"" << atts[i+1] << "\"";
					i += 2;
				}
				content << ">";
			}
			message->content.append(content.str());
		} else if(owner == "params") {
			// TODO <param>
			message->error(427, name);	// We don't support any <param> yet
			return;
		} else if(owner == "subscribe") {
			if(message->childs.back() == "dtmfsub") {
				bool all = true, collect = false, control = false;
//				if(!atts)	// No attributes, default is "all"
//					collect = control = true;
				if(atts) {
					int i=0;
					while(atts[i]) {	// Look for "matchmode"
						if(!atts[i])
							break;
						if(!strcmp(atts[i], "matchmode")) {
							if(!strcmp(atts[i+1], "all")) {
								all = true;
								collect = true;
								control = true;
							} else if(!strcmp(atts[i+1], "collect")) {
								all = false;
								collect = true;
								control = false;
							} else if(!strcmp(atts[i+1], "control")) {
								all = false;
								collect = false;
								control = true;
							} else {
								message->error(431, atts[i]);
								return;
							}
						} else {
							message->error(431, atts[i]);
							return;
						}
						i += 2;
					}
				}
				// TODO all != collect + control
				message->dialog->subscribe(all, collect, control);
			} else {
				message->error(431, name);	// We don't support this subscription
				return;
			}
		} else if(owner == "stream") {
			if(message->childs.back() == "region") {
				// This element has no attributes
				return;
			} else if(message->childs.back() == "priority") {
				// This element has no attributes
				return;
			} else {
				message->error(431, name);
				return;
			}
		} else {	// Only <dialog>, <param>, <stream> and <subscribe> are allowed to have children
			message->error(431, name);
			return;
		}
		return;
	} else {	// Deeper elements are not allowed (unless they're part of <dialog>)
		if(!message->digging) {
			message->error(431, name);
			return;
		} else {	// Every deeper element is domain of <dialog>: save it and parse it later
			// Save the whole subelement ant its attributes and children: we'll parse it later
			message->digging = true;
			stringstream content;
			if(!atts)	// No attributes
				content << "<" << name << ">";
			else {
				int i=0;
				content << "<" << name;
				while(atts[i]) {	// Write down all attributes
					if(!atts[i])
						break;
					content << " " << atts[i] << "=\"" << atts[i+1] << "\"";
					i += 2;
				}
				content << ">";
				
			}
			message->content.append(content.str());
		}
	}
}

static void XMLCALL valueElement(void *msg, const XML_Char *s, int len)
{
	IvrMessage *message = (IvrMessage *)msg;
	if(message->scanonly)
		return;
	if(message->stop)
		return;
	char value[len+1];
	int i=0;
	int pos = 0;
	while(i < len) {	// FIXME...
		if((s[i] == 0x09) || (s[i] == 0x0a) || (s[i] == 0x0d)) {
			i++;
			continue;
		} else if(s[i] == 0x20) {
			if(pos == 0) {	// Only skip backspaces *before* the content itself
				i++;
				continue;
			}
		}
		memcpy(value+pos, s+i, 1);
		i++;
		pos++;
	}
	if(pos == 0)
		return;
	// Now remove backspaces at the end
	i = pos;
	while(1) {
		i--;
		if(value[i] != 0x20);
			break;
		pos = i;
	}
	value[pos] = '\0';
	if(message->digging) {	// Is this element part of <dialog>?
		message->content.append(value);
		return;
	}
}

static void XMLCALL endElement(void *msg, const char *name)
{
	IvrMessage *message = (IvrMessage *)msg;
	if(message->scanonly)
		return;
	if(message->stop)	// Means that something was wrong...
		return;
	if(message->digging) {
		message->content.append("</");
		message->content.append(name);
		message->content.append(">");
		if(!strcmp(name, "dialog"))
			message->digging = false;
	}
	message->level--;
	message->childs.pop_back();
	if(message->level > 0)
		return;
	// We have come back to the root
	bool replied = false;

	if(message->content != "") {
		// Check if all in <dialog> is valid: it is in message->content, whether it was inline or externally referenced
		cout << "[IVR] Going to parse <dialog> content: " << message->content << endl;
		IvrMessage *newmsg = new IvrMessage();
		newmsg->blob = message->content;
		newmsg->sender = message->sender;
		// First scan the XML blob for errors...
		newmsg->scanonly = true;
		newmsg->digging = false;
		XML_Parser parser = XML_ParserCreate(NULL);
		XML_SetUserData(parser, newmsg);
		XML_SetElementHandler(parser, startElementBI, endElementBI);
		if (XML_Parse(parser, newmsg->blob.c_str(), newmsg->blob.length(), 1) == XML_STATUS_ERROR) {
			cout << "[IVR]     Error parsing <dialog> content: '"
				<< XML_ErrorString(XML_GetErrorCode(parser))
				<< "' at " << XML_GetCurrentLineNumber(parser) << ":"
				<< XML_GetCurrentColumnNumber(parser) << endl;
			cout << "[IVR]     Broken body was:" << endl << newmsg->blob << "(" << dec << newmsg->blob.length() << ")" << endl;
			message->error(400, "invalid 'dialog' XML body");
			XML_ParserFree(parser);
			return;
		}
		XML_ParserFree(parser);
		// ... and then actually parse and handle it
		newmsg->scanonly = false;
		newmsg->newdialog = message->newdialog;
		newmsg->dialog = message->dialog;
		newmsg->tid = message->tid;
		newmsg->pkg = message->pkg;
		newmsg->content = "";			// This content now refers to <grammar>
		newmsg->externalContent = false;	// 	""	""	""	""
		parser = XML_ParserCreate(NULL);
		XML_SetUserData(parser, newmsg);
		XML_SetElementHandler(parser, startElementBI, endElementBI);
		XML_SetCharacterDataHandler(parser, valueElementBI);
		XML_Parse(parser, newmsg->blob.c_str(), newmsg->blob.length(), 1);
		XML_ParserFree(parser);
		// Was the parsing ok?
		if(newmsg->stop) {
			message->stop = newmsg->stop;
			delete newmsg;
			return;
		}
		delete newmsg;
	}

	cout << "[IVR] Completed parsing of the request: " << message->request << endl;

	if(message->request == "audit") {	// An auditing request, no manipulation of dialogs is involved
		stringstream blob;
		blob //<< "<?xml version=\"1.0\"?>"
			<< "<mscivr version=\"1.0\" xmlns=\"urn:ietf:params:xml:ns:msc-ivr\">"
			<< "<auditresponse status=\"200\">";
		if(message->auditCapabilities) {
			// Capabilities
			blob << "<capabilities>"
			//	Codecs	(TODO ask the core; involve <params> where possible/needed)
				<< "<codecs>"
				<< "<codec><subtype>telephony-event</subtype></codec>"
				<< "<codec><subtype>PCMA</subtype></codec>"
				<< "<codec><subtype>PCMU</subtype></codec>"
				<< "<codec><subtype>GSM</subtype></codec>"
				<< "<codec><subtype>H.261</subtype></codec>"
				<< "<codec><subtype>H.263</subtype></codec>"
				<< "<codec><subtype>H.263+</subtype></codec>"
				<< "<codec><subtype>H.264</subtype></codec>"
				<< "</codecs>"
			//	DialogLanguages
				<< "<dialoglanguages/>"
			//	Grammars
				<< "<grammartypes/>"	// TODO
			//	Prompt types
				<< "<prompttypes>"	// TODO basically anything that ffmpeg supports...
				<< "<mimetype>audio/wav</mimetype>"
				<< "</prompttypes>"
			//	Record types
				<< "<recordtypes>"
				<< "<mimetype>audio/wav</mimetype>"
				<< "</recordtypes>"
			// Variables
				<< "<variables>"	// TODO variabletype
				<< "<variabletype type=\"date\" desc=\"value formatted as YYYY-MM-DD\">"
				<< "<format desc=\"month year day\">mdy</format>"
				<< "<format desc=\"year month day\">ymd</format>"
				<< "<format desc=\"day month year\">dmy</format>"
				<< "<format desc=\"day month\">dm</format>"
				<< "</variabletype>"
				<< "<variabletype type=\"time\" desc=\"value formatted as HH:MM\">"
				<< "<format desc=\"24 hour format\">t24</format>"
				<< "<format desc=\"12 hour format with am/pm\">t12</format>"
				<< "</variabletype>"
				<< "<variabletype type=\"digits\" desc=\"value formatted as D+\">"
				<< "<format desc=\"general digit string\">gen</format>"
				<< "<format desc=\"cardinal\">crn</format>"
				<< "<format desc=\"ordinal\">ord</format>"
				<< "</variabletype>"
				<< "</variables>"
			// max*
      			<< "<maxpreparedduration>60s</maxpreparedduration>"	// FIXME actually enforce this
      			<< "<maxrecordduration>1800s</maxrecordduration>"		// FIXME actually enforce this
			// The End
				<< "</capabilities>";
		}
		if(message->auditDialogs) {
			// Dialogs
			if(message->pkg->dialogs.empty())
				blob << "<dialogs/>";
			else {
				blob << "<dialogs>";
				map<string, IvrDialog *>::iterator iter;
				IvrDialog *dialog = NULL;
				for(iter = message->pkg->dialogs.begin(); iter != message->pkg->dialogs.end(); iter++) {
					dialog = iter->second;
					if(dialog == NULL)
						continue;
					if((message->auditDialog != "") && (message->auditDialog != iter->first))
						continue;	// Not the dialog we're interested to
					if(!dialog->checkSender(message->sender)) {	// This dialog was not created by this requester
						if(message->auditDialog == "")
							continue;	// It's ok, we just skip this dialog...
						message->pkg->callback->report(message->pkg, message->sender, message->tid, 403, 0);			// FIXME
						message->stop = true;
						replied = true;
						return;
					}
					int state = dialog->getState();
					if(state == DIALOG_IDLE)
						blob << "<dialogaudit dialogid=\"" << iter->first << "\" state=\"idle\"";
					else if(state == DIALOG_PREPARING)
						blob << "<dialogaudit dialogid=\"" << iter->first << "\" state=\"preparing\"";
					else if(state == DIALOG_PREPARED)
						blob << "<dialogaudit dialogid=\"" << iter->first << "\" state=\"prepared\"";
					else if(state == DIALOG_STARTING) {
						blob << "<dialogaudit dialogid=\"" << iter->first << "\" state=\"starting\"";
						ControlPackageConnection *connection = dialog->getConnection();
						if(connection != NULL) {
							if(connection->getType() == CPC_CONNECTION)
								blob << " connectionid=\"" << connection->getConnectionId() << "\"";
							else if(connection->getType() == CPC_CONFERENCE)
								blob << " conferenceid=\"" << connection->getConnectionId() << "\"";
						}
						blob << "/>";
					} else if(state == DIALOG_STARTED) {
						blob << "<dialogaudit dialogid=\"" << iter->first << "\" state=\"started\"";
						ControlPackageConnection *connection = dialog->getConnection();
						if(connection != NULL) {
							if(connection->getType() == CPC_CONNECTION)
								blob << " connectionid=\"" << connection->getConnectionId() << "\"";
							else if(connection->getType() == CPC_CONFERENCE)
								blob << " conferenceid=\"" << connection->getConnectionId() << "\"";
						}
						blob << "/>";
					} else if(state == DIALOG_TERMINATED)
						blob << "<dialogaudit dialogid=\"" << iter->first << "\" state=\"terminated\"";
				}
				blob << "</dialogs>";
			}
		}
		// We're done
		blob << "</auditresponse></mscivr>";
		message->pkg->callback->report(message->pkg, message->sender, message->tid, 200, 10, blob.str());
		replied = true;
	} else {	// Something related to dialogs was requested (e.g. prepare/start/terminate)
		if(message->request == "dialogterminate") {	// Check a termination request first
			cout << "[IVR]     Destroying dialog:            " << message->dialog->getDialogId() << endl;
			cout << "[IVR]     Immediate destruction:        " << (message->immediate ? "yes" : "no") << endl;
	//		message->pkg->dialogs.erase(message->dialog->getDialogId());
			message->dialog->destroy(message->immediate, DIALOGEXIT_DIALOGTERMINATE);
			if(!replied) {		// FIXME Reply is different according to 'immediate' value...
				stringstream blob;
				blob //<< "<?xml version=\"1.0\"?>"
					<< "<mscivr version=\"1.0\" xmlns=\"urn:ietf:params:xml:ns:msc-ivr\">"
					<< "<response status=\"200\" "
					<< "reason=\"Dialog terminated\" "
					<< "dialogid=\"" << message->dialog->getDialogId() << "\""
					<< "/>"
					<< "</mscivr>";
				message->pkg->callback->report(message->pkg, message->sender, message->tid, 200, 10, blob.str());
				replied = true;
			}
			return;
		}
	
		if(message->dialog->getState() == DIALOG_IDLE) {		// Prepare the new dialog
			cout << "[IVR]     Created new dialog: " << message->dialog->getDialogId() << endl;
			cout << "[IVR]         Model:          " << message->dialog->getModelString() << endl;
			int err = message->dialog->prepare();
			if(err != 0) {
				// TODO Check error response and send related code
				message->error(err, message->dialog->errorString.str());
				replied = true;
			} else {
				if(message->request == "dialogprepare") {	// We're done
					stringstream blob;
					blob //<< "<?xml version=\"1.0\"?>"
						<< "<mscivr version=\"1.0\" xmlns=\"urn:ietf:params:xml:ns:msc-ivr\">"
						<< "<response status=\"200\" "
						<< "reason=\"Dialog prepared\" "
						<< "dialogid=\"" << message->dialog->getDialogId() << "\""
						<< "/>"
						<< "</mscivr>";
					message->pkg->dialogs[message->dialog->getDialogId()] = message->dialog;
					message->pkg->callback->report(message->pkg, message->sender, message->tid, 200, 10, blob.str());
					replied = true;
				}
			}
		}
		if(message->dialog->getState() == DIALOG_PREPARED) {	// Start an already prepared dialog
			if(message->request == "dialogstart") {	// Only do it if the request was a 'dialogstart'
				if(message->conntype == CONNECTION)
					cout << "[IVR]         Connection-ID:  " << message->connId << endl;
				else if(message->conntype == CONFERENCE)
					cout << "[IVR]         Conf-ID:        " << message->connId << endl;
				// FIXME FIXME FIXME
				ControlPackageConnection *dialogConnection = message->pkg->callback->getConnection(message->pkg, message->connId);
				if(dialogConnection == NULL) {
					cout << "[IVR]     Error accessing connection " << message->connId << endl;
					message->error((message->conntype == CONNECTION ? 407 : 408), message->connId);
					replied = true;
				} else if((message->conntype == CONNECTION) && (dialogConnection->getType() != CPC_CONNECTION)) {
					cout << "[IVR]     Requested a Connection (connectionid) and got a Conference instead! (" << message->connId << ")" << endl;
					message->error((message->conntype == CONNECTION ? 407 : 408), message->connId);
					replied = true;
				} else if((message->conntype == CONFERENCE) && (dialogConnection->getType() != CPC_CONFERENCE)) {
					cout << "[IVR]     Requested a Conference (conferenceid) and got a Connection instead! (" << message->connId << ")" << endl;
					message->error((message->conntype == CONNECTION ? 407 : 408), message->connId);
					replied = true;
				}
				if(!replied && !message->streams.empty()) {
					cout << "[IVR]         Streams:" << endl;
					list<IvrStream *>::iterator iter;
					for(iter = message->streams.begin(); iter != message->streams.end(); iter++ ) {
						cout << "[IVR]             " << (*iter)->media << " --> " << ((*iter)->label != "" ? (*iter)->label : "no explicit label provided") << " (" << (*iter)->getDirection() << ")" << endl;
						if(dialogConnection->getType() == CPC_CONFERENCE) {	// It's a conference
							// TODO
						} else if(dialogConnection->getMediaType() != MEDIACTRL_MEDIA_UNKNOWN) {	// Check if this stream conflicts with the explicit connection label
							if(dialogConnection->getMediaType() != (*iter)->mediaType) {
								cout << "[IVR]     Conflicting stream media (" << dialogConnection->getLabel() << " is not " << (*iter)->media << ")" << endl;
								message->error(411, dialogConnection->getLabel() + " is not " + (*iter)->media);
								replied = true;
								break;
							}
							if(((*iter)->label != "") && (dialogConnection->getLabel() != (*iter)->label)) {
								cout << "[IVR]     Conflicting stream label (should have been " << dialogConnection->getLabel() << ")" << endl;
								message->error(411, (*iter)->label + " != " + dialogConnection->getLabel());
								replied = true;
								break;
							}
						} else {	// Check if the stream label actually belongs to the connection, and if it's of the right media type
							ControlPackageConnection *streamConnection = NULL;
							if((*iter)->label != "")
								streamConnection = message->pkg->callback->getSubConnection(dialogConnection, (*iter)->label);
							else
								streamConnection = message->pkg->callback->getSubConnection(dialogConnection, (*iter)->mediaType);
							if(streamConnection == NULL) {	// This label does not exist
								if((*iter)->label != "") {
									cout << "[IVR]     stream label does not exist for connection " << message->connId << endl;
									message->error(411, (*iter)->label + " does not exist");
									replied = true;
									break;
								} else {
									cout << "[IVR]     stream media does not exist for connection " << message->connId << endl;
									message->error(411, (*iter)->media + " does not exist");
									replied = true;
									break;
								}
							} else {
								(*iter)->connection = streamConnection;
								if(streamConnection->getMediaType() != (*iter)->mediaType) {
									cout << "[IVR]     Conflicting stream media (" << (*iter)->label << " is not " << (*iter)->media << ")" << endl;
									message->error(411, (*iter)->label + " is not " + (*iter)->media);
									replied = true;
									break;
								}
								if(((*iter)->label != "") && (streamConnection->getLabel() != (*iter)->label)) {
									cout << "[IVR]     Conflicting stream label (should have been " << streamConnection->getLabel() << ")" << endl;
									message->error(411, (*iter)->label + " != " + streamConnection->getLabel());
									replied = true;
									break;
								}
							}
						}
					}
					if(!replied)
						message->dialog->streams = message->streams;
					else {	// Something went wrong...
						while(!message->streams.empty()) {
							IvrStream *stream = message->streams.front();
							message->streams.pop_front();
							delete stream;
						}
					}
				}
				if(!replied && (message->dialog->attachConnection() < 0)) {
					cout << "[IVR]     Error attaching connection " << message->connId << endl;
					message->error((message->conntype == CONNECTION ? 407 : 408), message->connId);
					replied = true;
				}
				if(!replied) {	// Everything went fine
					cout << "[IVR]     Starting dialog:              " << message->dialog->getDialogId() << endl;
					message->pkg->dialogs[message->dialog->getDialogId()] = message->dialog;
					message->dialog->start();	// FIXME
					if(!replied) {
						stringstream blob;
						blob //<< "<?xml version=\"1.0\"?>"
							<< "<mscivr version=\"1.0\" xmlns=\"urn:ietf:params:xml:ns:msc-ivr\">"
							<< "<response status=\"200\" "
							<< "reason=\"Dialog started\" "
							<< "dialogid=\"" << message->dialog->getDialogId() << "\""
							<< "/>"
							<< "</mscivr>";
						message->pkg->callback->report(message->pkg, message->sender, message->tid, 200, message->dialog->getTimeout()+10, blob.str());
						replied = true;
					}
				}
			}
		}
	}
}


// eXpat callbacks handle the parsing of <dialog> subelements
static void XMLCALL startElementBI(void *msg, const char *name, const char **atts)
{
	IvrMessage *message = (IvrMessage *)msg;
	if(message->scanonly)
		return;
	if(message->stop)
		return;
	if(message->level == 0) {
		// FIXME do some initialization here? it's the root
	}
	message->level++;
	message->childs.push_back(name);
	if(message->level == 1) {
		if(message->childs.back() != "dialog") {	// dialog
			message->error(400, name);
			return;
		}
		string repeatCount = "", repeatDur = "";
		int i = 0;
		while(atts[i]) {	// We don't care about other attributes
			if(!atts[i])
				break;
			if(!strcmp(atts[i], "repeatCount"))
				repeatCount = atts[i+1];
			else if(!strcmp(atts[i], "repeatDur"))
				repeatDur = atts[i+1];
			else {
				message->error(431, atts[i]);
				return;
			}
			i += 2;
		}
		bool ok;
		uint32_t iterations = 1;
		if(repeatCount != "") {
			iterations = positiveInteger(repeatCount, &ok);
			if(!ok) {		// Invalid value for attribute
				message->error(400, "repeatCount");
				return;
			}
			if(iterations == 0)		// Infinite
				iterations = ULONG_MAX-1;
		}
		message->dialog->setIterations(iterations);
		uint32_t duration = 0;
		if(repeatDur != "") {
			duration = timeDesignation(repeatDur, &ok);
			if(!ok) {		// Invalid value for attribute
				message->error(400, "repeatDur");
				return;
			}
		}
		message->dialog->setDuration(duration);
	} else if(message->level == 2) {	// prompt/collect/control/record
		if(message->childs.back() == "prompt") {
			if(message->dialog->getModel() & MODEL_PROMPT) {
				message->error(400, "multiple 'prompt' elements defined");	// FIXME Which error is better?
				return;
			}
			message->dialog->addModel(MODEL_PROMPT);
			if(!atts)	// no attributes were specified
				return;
			int i = 0;
			int errorCode;
			string error = "";
			while(atts[i]) {
				if(!atts[i])
					break;
				// Parse and check every attribute
				errorCode = 0;
				error = message->dialog->checkDataParameter(MODEL_PROMPT, atts[i], atts[i+1], &errorCode);
				if(errorCode != 200) {	// Error...
					message->error(errorCode, error);
					return;
				}
				i += 2;
			}
		} else if(message->childs.back() == "collect") {
			if(message->dialog->getModel() & MODEL_COLLECT) {
				message->error(400, "multiple 'collect' elements defined");	// FIXME Which error is better?
				return;
			}
			message->dialog->addModel(MODEL_COLLECT);
			if(!atts)	// no attributes were specified
				return;
			int i = 0;
			int errorCode;
			string error = "";
			while(atts[i]) {
				if(!atts[i])
					break;
				// Parse and check every attribute
				errorCode = 0;
				error = message->dialog->checkDataParameter(MODEL_COLLECT, atts[i], atts[i+1], &errorCode);
				if(errorCode != 200) {	// Error...
					message->error(errorCode, error);
					return;
				}
				i += 2;
			}
		} else if(message->childs.back() == "control") {
			if(message->dialog->getModel() & MODEL_CONTROL) {
				message->error(400, "multiple 'control' elements defined");	// FIXME Which error is better?
				return;
			}
			message->dialog->addModel(MODEL_CONTROL);
			if(!atts)	// no attributes were specified
				return;
			int i = 0;
			int errorCode;
			string error = "";
			while(atts[i]) {
				if(!atts[i])
					break;
				// Parse and check every attribute
				errorCode = 0;
				error = message->dialog->checkDataParameter(MODEL_CONTROL, atts[i], atts[i+1], &errorCode);
				if(errorCode != 200) {	// Error...
					message->error(errorCode, error);
					return;
				}
				i += 2;
			}
		} else if(message->childs.back() == "record") {
			if(message->dialog->getModel() & MODEL_RECORD) {
				message->error(400, "multiple 'record' elements defined");	// FIXME Which error is better?
				return;
			}
			message->dialog->addModel(MODEL_RECORD);
			if(!atts)	// no attributes were specified
				return;
			int i = 0;
			int errorCode;
			string error = "";
			while(atts[i]) {
				if(!atts[i])
					break;
				// Parse and check every attribute
				errorCode = 0;
				error = message->dialog->checkDataParameter(MODEL_RECORD, atts[i], atts[i+1], &errorCode);
				if(errorCode != 200) {	// Error...
					message->error(errorCode, error);
					return;
				}
				i += 2;
			}
		} else {
			message->error(431, name);
			return;
		}
	} else if(message->level == 3) {	// media/variable/dtmf/grammar
		message->childs.pop_back();
		string owner = message->childs.back();
		message->childs.push_back(name);
		if(owner == "prompt") {		// <prompt> supports <media>, <variable> and <dtmf>
			// TODO If no children are provided, a 400 must be triggered
			// TODO If prompt retrieval exceeds timeout, it's a 409 error
			message->dialog->newTimelineStep = true;
			message->dialog->newSlot = true;
			if(message->childs.back() == "media") {
				message->dialog->currentEndSync = ENDSYNC_NONE;
				if(!atts) {	// no attributes were specified, it's an error (loc is mandatory)
					message->error(400, "loc");
					return;
				}
				int i = 0;
				string src = "", fetchtimeout = "";
				uint32_t clipBegin = 0, clipEnd = 0;
				uint16_t soundLevel = 100;
				while(atts[i]) {
					if(!atts[i])
						break;
					if(!strcmp(atts[i], "loc"))
						src = atts[i+1];
					else if(!strcmp(atts[i], "fetchtimeout"))
						fetchtimeout = atts[i+1];
					else if(!strcmp(atts[i], "type"))
						// TODO Handle media type, we ignore it currently...
						cout << "[IVR] *** we ignore type currently... (" << atts[i+1] << ")" << endl;
					else if(!strcmp(atts[i], "soundLevel")) {
						bool ok;
						soundLevel = percentValue(atts[i+1], &ok);
						if(!ok) {
							message->error(400, "soundLevel");
							return;
						}
					} else if(!strcmp(atts[i], "clipBegin")) {
						bool ok;
						clipBegin = timeDesignation(atts[i+1], &ok);
						if(!ok) {
							message->error(400, "clipBegin");
							return;
						}
					} else if(!strcmp(atts[i], "clipEnd")) {
						bool ok;
						clipEnd = timeDesignation(atts[i+1], &ok);
						if(!ok) {
							message->error(400, "clipEnd");
							return;
						}
					} else {
						message->error(431, atts[i]);
						return;
					}
					i += 2;
				}
				if(src == "") {	// src was not specified, it's an error (src is mandatory)
					message->error(400, "loc");
					return;
				}
				bool ok;
				if(fetchtimeout == "")
					fetchtimeout = "30s";
				uint32_t timeout = timeDesignation(fetchtimeout, &ok);
				if(!ok) {
					message->error(400, "fetchtimeout");
					return;
				}
				if(clipBegin > clipEnd) {
					message->error(400, "clipBegin > clipEnd");
					return;
				}
				if((clipBegin == clipEnd) && (clipBegin > 0)) {
					message->error(400, "clipBegin = clipEnd");
					return;
				}
				i = message->dialog->addFilename(TIMELINE_NORMAL, src, timeout, soundLevel, clipBegin, clipEnd);
				if(i < 0) {
					message->error(400, src);	// FIXME
					return;
				} else if(i > 0) {
					message->error(i, src);
					return;
				}
			} else if(message->childs.back() == "variable") {
				message->dialog->currentEndSync = ENDSYNC_NONE;
				if(!atts) {	// no attributes were specified, it's an error (value and type are mandatory)
					message->error(400, "value/type");
					return;
				}
				int i = 0;
				string value = "", type = "", format = "";
				while(atts[i]) {
					if(!atts[i])
						break;
					if(!strcmp(atts[i], "value"))
						value = atts[i+1];
					else if(!strcmp(atts[i], "type"))
						type = atts[i+1];
					else if(!strcmp(atts[i], "format"))
						format = atts[i+1];
					else if(!strcmp(atts[i], "gender"))
						// TODO Handle media gender, we ignore it currently...
						cout << "[IVR] *** we ignore gender currently... (" << atts[i+1] << ")" << endl;
					else if(!strcmp(atts[i], "xml:lang"))
						// TODO Handle media language, we ignore it currently...
						cout << "[IVR] *** we ignore xml:lang currently... (" << atts[i+1] << ")" << endl;
					else {
						message->error(431, atts[i]);
						return;
					}
					i += 2;
				}
				if(value == "") {	// value was not specified specified, it's an error (value is mandatory)
					message->error(400, "value");
					return;
				}
				if(type == "") {	// type was not specified specified, it's an error (type is mandatory)
					message->error(400, "type");
					return;
				}
				// Build filenames out of the variable
				cout << "[IVR] Building filenames out of 'variable': value=" << value << ", type=" << type << ", format=" << format << endl;
				list<string> *filenames = buildVariableFilenames(value, type, format);
				if(filenames == NULL) {	// FIXME
					message->error(425, "variable");
					return;
				}
				list<string>::iterator iter;
				for(iter = filenames->begin(); iter != filenames->end(); iter++) {
					cout << "[IVR] \t" << (*iter) << endl;
//					message->dialog->addFilename(TIMELINE_NORMAL, (*iter), 30000, 100, 0, 0);	// FIXME
					i = message->dialog->addFilename(TIMELINE_SEQ, (*iter), 30000, 100, 0, 0);	// FIXME
					if(i < 0) {
						message->error(400, value);	// FIXME
						return;
					} else if(i > 0) {
						message->error(i, value);
						return;
					}
					message->dialog->newSlot = false;
					message->dialog->newTimelineStep = false;
				}
			} else if(message->childs.back() == "dtmf") {
				message->error(426, "digits");
				// TODO implement <dtmf>
				message->dialog->currentEndSync = ENDSYNC_NONE;
				if(!atts) {	// no attributes were specified, it's an error (digits is mandatory)
					message->error(400, "digits");
					return;
				}
				int i = 0;
				string digits = "";
				while(atts[i]) {
					if(!atts[i])
						break;
					if(!strcmp(atts[i], "digits")) {
						// TODO Handle dtmf digits, we ignore it currently...
						cout << "[IVR] *** we ignore type currently, except for checking it (mandatory attribute)..." << endl;
						digits = atts[i+1];
					} else if(!strcmp(atts[i], "level"))
						// TODO Handle dtmf level, we ignore it currently...
						cout << "[IVR] *** we ignore dtmf::level currently..." << endl;
					else if(!strcmp(atts[i], "duration"))
						// TODO Handle dtmf duration, we ignore it currently...
						cout << "[IVR] *** we ignore dtmf::duration currently..." << endl;
					else if(!strcmp(atts[i], "interval"))
						// TODO Handle dtmf interval, we ignore it currently...
						cout << "[IVR] *** we ignore dtmf::interval currently..." << endl;
					else {
						message->error(431, atts[i]);
						return;
					}
					i += 2;
				}
				if(digits == "") {	// no attributes were specified, it's an error (digits is mandatory)
					message->error(400, "digits");
					return;
				}
			} else if(message->childs.back() == "par") {
				message->dialog->currentEndSync = ENDSYNC_LAST;
				string endSync = "";
				if(atts != NULL) {
					int i = 0;
					string value = "";
					while(atts[i]) {
						if(!atts[i])
							break;
						if(!strcmp(atts[i], "endsync")) {
							endSync = atts[i+1];
						} else {
							message->error(431, atts[i]);
							return;
						}
						i += 2;
					}
				}
				if(endSync != "") {
					regex re;
					re.assign("first|last", regex_constants::icase);
					if(!regex_match(endSync.c_str(), re)) {
						message->error(400, "endsync");
						return;
					}
					re.assign("first", regex_constants::icase);
					if(regex_match(endSync.c_str(), re))
						message->dialog->currentEndSync = ENDSYNC_FIRST;
					else
						message->dialog->currentEndSync = ENDSYNC_LAST;
				}
				// TODO Implement <par>
				return;
			} else {
				message->error(431, name);
				return;
			}
		} else if(owner == "collect") {	// <collect> only supports <grammar> (but we don't anyway... FIXME)
			if(message->childs.back() == "grammar") {
				string src = "", type = "", fetchtimeout = "";
				int i = 0;
				while(atts[i]) {
					if(!atts[i])
						break;
					if(!strcmp(atts[i], "src"))
						src = atts[i+1];
					else if(!strcmp(atts[i], "type"))
						type = atts[i+1];
					else if(!strcmp(atts[i], "fetchtimeout"))
						fetchtimeout = atts[i+1];
					else {
						message->error(431, atts[i]);
						return;
					}
					i += 2;
				}
				if((type != "") && (type != "application/srgs+xml") < 0) {
					message->error(424, type);	// Unsupported grammar format
					return;
				}
				// Get the XML blob referenced by the URL in 'src', if specified
				if(src != "") {
					message->externalContent = true;
					bool ok;
					if(fetchtimeout == "")
						fetchtimeout = "30s";
					uint32_t timeout = timeDesignation(fetchtimeout, &ok);
					if(!ok) {
						message->error(400, "fetchtimeout");
						return;
					}
					// TODO We first have to check if the URI is supported (error 420 otherwise)
					long int code = 0;
					message->content = getExternalReference(src, timeout, &code);
					if(message->content == "") {
						stringstream errorCode;
						errorCode << "Error " << dec << code << " for " << src;
						message->error(409, errorCode.str());
						return;
					}
				}
			} else {
				message->error(431, name);
				return;
			}
//			message->error(424, name);
			return;
		} else if(owner == "record") {	// <record> supports a crippled version of <media>
			if(message->childs.back() == "media") {
				if(!atts) {	// no attributes were specified, it's an error (loc is mandatory)
					message->error(400, "loc");
					return;
				}
				int i = 0;
				string src = "", type = "";
				while(atts[i]) {
					if(!atts[i])
						break;
					if(!strcmp(atts[i], "loc"))
						src = atts[i+1];
					else if(!strcmp(atts[i], "type"))
						type = atts[i+1];
					else if(!strcmp(atts[i], "soundLevel")) {
						i += 2;
						continue;	// Ignore
					} else if(!strcmp(atts[i], "clipBegin")) {
						i += 2;
						continue;	// Ignore
					} else if(!strcmp(atts[i], "clipEnd")) {
						i += 2;
						continue;	// Ignore
					} else {
						message->error(431, atts[i]);
						return;
					}
					i += 2;
				}
				if(src == "") {	// src was not specified, it's an error (src is mandatory)
					message->error(400, "loc");
					return;
				}
				// TODO Check if dest is writable
				if(type != "") {
					regex re;
					re.assign("audio/x-wav", regex_constants::icase);
					if(!regex_match(type.c_str(), re)) {
						message->error(423, type);	// FIXME Unsupported recording format (we actually rely on ffmpeg, so this should be less restrictive...)
						return;
					} else
						message->dialog->addRecordingType(src, true, "audio/x-wav");	// FIXME
				}
			} else {
				message->error(431, name);
				return;
			}
		} else {	// ??
			message->error(431, name);
			return;
		}
	} else if(message->level == 4) {	// <par> --> <seq>/<media>/<variable>/<dtmf>, <grammar>
		message->childs.pop_back();
		string owner = message->childs.back();
		message->childs.push_back(name);
		if(owner == "par") {		// <par> supports <media>, <variable>, <dtmf> and <seq>
			// TODO If no children are provided, a 400 must be triggered
			// TODO If prompt retrieval exceeds timeout, it's a 409 error
			message->dialog->newSlot = true;
			if(message->childs.back() == "media") {
				if(!atts) {	// no attributes were specified, it's an error (loc is mandatory)
					message->error(400, "loc");
					return;
				}
				int i = 0;
				string src = "", fetchtimeout = "";
				uint32_t clipBegin = 0, clipEnd = 0;
				uint16_t soundLevel = 100;
				while(atts[i]) {
					if(!atts[i])
						break;
					if(!strcmp(atts[i], "loc"))
						src = atts[i+1];
					else if(!strcmp(atts[i], "fetchtimeout"))
						fetchtimeout = atts[i+1];
					else if(!strcmp(atts[i], "type"))
						// TODO Handle media type, we ignore it currently...
						cout << "[IVR] *** we ignore type currently... (" << atts[i+1] << ")" << endl;
					else if(!strcmp(atts[i], "soundLevel")) {
						bool ok;
						soundLevel = percentValue(atts[i+1], &ok);
						if(!ok) {
							message->error(400, "soundLevel");
							return;
						}
					} else if(!strcmp(atts[i], "clipBegin")) {
						bool ok;
						clipBegin = timeDesignation(atts[i+1], &ok);
						if(!ok) {
							message->error(400, "clipBegin");
							return;
						}
					} else if(!strcmp(atts[i], "clipEnd")) {
						bool ok;
						clipEnd = timeDesignation(atts[i+1], &ok);
						if(!ok) {
							message->error(400, "clipEnd");
							return;
						}
					} else {
						message->error(431, atts[i]);
						return;
					}
					i += 2;
				}
				if(src == "") {	// src was not specified, it's an error (src is mandatory)
					message->error(400, "loc");
					return;
				}
				bool ok;
				if(fetchtimeout == "")
					fetchtimeout = "30s";
				uint32_t timeout = timeDesignation(fetchtimeout, &ok);
				if(!ok) {
					message->error(400, "fetchtimeout");
					return;
				}
				if(clipBegin > clipEnd) {
					message->error(400, "clipBegin > clipEnd");
					return;
				}
				if((clipBegin == clipEnd) && (clipBegin > 0)) {
					message->error(400, "clipBegin = clipEnd");
					return;
				}
				i = message->dialog->addFilename(TIMELINE_PAR, src, timeout, soundLevel, clipBegin, clipEnd);	// FIXME
				if(i < 0) {
					message->error(400, src);	// FIXME
					return;
				} else if(i > 0) {
					message->error(i, src);
					return;
				}
				message->dialog->newTimelineStep = false;
			} else if(message->childs.back() == "variable") {
				if(!atts) {	// no attributes were specified, it's an error (value and type are mandatory)
					message->error(400, "value/type");
					return;
				}
				int i = 0;
				string value = "", type = "", format = "";
				while(atts[i]) {
					if(!atts[i])
						break;
					if(!strcmp(atts[i], "value"))
						value = atts[i+1];
					else if(!strcmp(atts[i], "type"))
						type = atts[i+1];
					else if(!strcmp(atts[i], "format"))
						format = atts[i+1];
					else if(!strcmp(atts[i], "gender"))
						// TODO Handle media gender, we ignore it currently...
						cout << "[IVR] *** we ignore gender currently... (" << atts[i+1] << ")" << endl;
					else if(!strcmp(atts[i], "xml:lang"))
						// TODO Handle media language, we ignore it currently...
						cout << "[IVR] *** we ignore xml:lang currently... (" << atts[i+1] << ")" << endl;
					else {
						message->error(431, atts[i]);
						return;
					}
					i += 2;
				}
				if(value == "") {	// value was not specified specified, it's an error (value is mandatory)
					message->error(400, "value");
					return;
				}
				if(type == "") {	// type was not specified specified, it's an error (type is mandatory)
					message->error(400, "type");
					return;
				}
				// Build filenames out of the variable
				cout << "[IVR] Building filenames out of 'variable': value=" << value << ", type=" << type << ", format=" << format << endl;
				list<string> *filenames = buildVariableFilenames(value, type, format);
				if(filenames == NULL) {	// FIXME
					message->error(425, "variable");
					return;
				}
				list<string>::iterator iter;
				for(iter = filenames->begin(); iter != filenames->end(); iter++) {
					cout << "[IVR] \t" << (*iter) << endl;
					i = message->dialog->addFilename(TIMELINE_SEQ, (*iter), 30000, 100, 0, 0);	// FIXME
					if(i < 0) {
						message->error(400, value);	// FIXME
						return;
					} else if(i > 0) {
						message->error(i, value);
						return;
					}
					message->dialog->newSlot = false;
					message->dialog->newTimelineStep = false;
				}
			} else if(message->childs.back() == "dtmf") {
				message->error(426, "digits");
				// TODO implement <dtmf>
				if(!atts) {	// no attributes were specified, it's an error (digits is mandatory)
					message->error(400, "digits");
					return;
				}
				int i = 0;
				string digits = "";
				while(atts[i]) {
					if(!atts[i])
						break;
					if(!strcmp(atts[i], "digits")) {
						// TODO Handle dtmf digits, we ignore it currently...
						cout << "[IVR] *** we ignore type currently, except for checking it (mandatory attribute)..." << endl;
						digits = atts[i+1];
					} else if(!strcmp(atts[i], "level"))
						// TODO Handle dtmf level, we ignore it currently...
						cout << "[IVR] *** we ignore dtmf::level currently..." << endl;
					else if(!strcmp(atts[i], "duration"))
						// TODO Handle dtmf duration, we ignore it currently...
						cout << "[IVR] *** we ignore dtmf::duration currently..." << endl;
					else if(!strcmp(atts[i], "interval"))
						// TODO Handle dtmf interval, we ignore it currently...
						cout << "[IVR] *** we ignore dtmf::interval currently..." << endl;
					else {
						message->error(431, atts[i]);
						return;
					}
					i += 2;
				}
				if(digits == "") {	// no attributes were specified, it's an error (digits is mandatory)
					message->error(400, "digits");
					return;
				}
			} else if(message->childs.back() == "seq") {
				// TODO Implement <seq>
				return;
			}
		} else if(owner == "grammar") {
			if(message->externalContent) {
				message->error(400, "can't have both src AND inline grammar stuff");
				return;
			}
			message->digging = true;
			// Save the whole subelement ant its attributes and children: we'll parse it later
			stringstream content;
			if(!atts)	// No attributes
				content << "<grammar>";
			else {
				int i=0;
				content << "<grammar";
				while(atts[i]) {	// Write down all attributes
					if(!atts[i])
						break;
					content << " " << atts[i] << "=\"" << atts[i+1] << "\"";
					i += 2;
				}
				content << ">";
			}
			message->content.append(content.str());
		} else {	// ??
			message->error(431, name);
			return;
		}
	} else if(message->level == 5) {	// <seq> --> <media>/<variable>/<dtmf>
		message->childs.pop_back();
		string owner = message->childs.back();
		message->childs.push_back(name);
		if(owner == "seq") {		// <seq> supports <media>, <variable> and <dtmf>
			// TODO If no children are provided, a 400 must be triggered
			// TODO If prompt retrieval exceeds timeout, it's a 409 error
			if(message->childs.back() == "media") {
				if(!atts) {	// no attributes were specified, it's an error (loc is mandatory)
					message->error(400, "loc");
					return;
				}
				int i = 0;
				string src = "", fetchtimeout = "";
				uint32_t clipBegin = 0, clipEnd = 0;
				uint16_t soundLevel = 100;
				while(atts[i]) {
					if(!atts[i])
						break;
					if(!strcmp(atts[i], "loc"))
						src = atts[i+1];
					else if(!strcmp(atts[i], "fetchtimeout"))
						fetchtimeout = atts[i+1];
					else if(!strcmp(atts[i], "type"))
						// TODO Handle media type, we ignore it currently...
						cout << "[IVR] *** we ignore type currently... (" << atts[i+1] << ")" << endl;
					else if(!strcmp(atts[i], "soundLevel")) {
						bool ok;
						soundLevel = percentValue(atts[i+1], &ok);
						if(!ok) {
							message->error(400, "soundLevel");
							return;
						}
					} else if(!strcmp(atts[i], "clipBegin")) {
						bool ok;
						clipBegin = timeDesignation(atts[i+1], &ok);
						if(!ok) {
							message->error(400, "clipBegin");
							return;
						}
					} else if(!strcmp(atts[i], "clipEnd")) {
						bool ok;
						clipEnd = timeDesignation(atts[i+1], &ok);
						if(!ok) {
							message->error(400, "clipEnd");
							return;
						}
					} else {
						message->error(431, atts[i]);
						return;
					}
					i += 2;
				}
				if(src == "") {	// src was not specified, it's an error (src is mandatory)
					message->error(400, "loc");
					return;
				}
				bool ok;
				if(fetchtimeout == "")
					fetchtimeout = "30s";
				uint32_t timeout = timeDesignation(fetchtimeout, &ok);
				if(!ok) {
					message->error(400, "fetchtimeout");
					return;
				}
				if(clipBegin > clipEnd) {
					message->error(400, "clipBegin > clipEnd");
					return;
				}
				if((clipBegin == clipEnd) && (clipBegin > 0)) {
					message->error(400, "clipBegin = clipEnd");
					return;
				}
				i = message->dialog->addFilename(TIMELINE_SEQ, src, timeout, soundLevel, clipBegin, clipEnd);	// FIXME
				if(i < 0) {
					message->error(400, src);	// FIXME
					return;
				} else if(i > 0) {
					message->error(i, src);
					return;
				}
				message->dialog->newSlot = false;
				message->dialog->newTimelineStep = false;
			} else if(message->childs.back() == "variable") {
				if(!atts) {	// no attributes were specified, it's an error (value and type are mandatory)
					message->error(400, "value/type");
					return;
				}
				int i = 0;
				string value = "", type = "", format = "";
				while(atts[i]) {
					if(!atts[i])
						break;
					if(!strcmp(atts[i], "value"))
						value = atts[i+1];
					else if(!strcmp(atts[i], "type"))
						type = atts[i+1];
					else if(!strcmp(atts[i], "format"))
						format = atts[i+1];
					else if(!strcmp(atts[i], "gender"))
						// TODO Handle media gender, we ignore it currently...
						cout << "[IVR] *** we ignore gender currently... (" << atts[i+1] << ")" << endl;
					else if(!strcmp(atts[i], "xml:lang"))
						// TODO Handle media language, we ignore it currently...
						cout << "[IVR] *** we ignore xml:lang currently... (" << atts[i+1] << ")" << endl;
					else {
						message->error(431, atts[i]);
						return;
					}
					i += 2;
				}
				if(value == "") {	// value was not specified specified, it's an error (value is mandatory)
					message->error(400, "value");
					return;
				}
				if(type == "") {	// type was not specified specified, it's an error (type is mandatory)
					message->error(400, "type");
					return;
				}
				// Build filenames out of the variable
				cout << "[IVR] Building filenames out of 'variable': value=" << value << ", type=" << type << ", format=" << format << endl;
				list<string> *filenames = buildVariableFilenames(value, type, format);
				if(filenames == NULL) {	// FIXME
					message->error(425, "variable");
					return;
				}
				list<string>::iterator iter;
				for(iter = filenames->begin(); iter != filenames->end(); iter++) {
					cout << "[IVR] \t" << (*iter) << endl;
					i = message->dialog->addFilename(TIMELINE_SEQ, (*iter), 30000, 100, 0, 0);	// FIXME
					if(i < 0) {
						message->error(400, value);	// FIXME
						return;
					} else if(i > 0) {
						message->error(i, value);
						return;
					}
					message->dialog->newSlot = false;
					message->dialog->newTimelineStep = false;
				}
			} else if(message->childs.back() == "dtmf") {
				message->error(426, "digits");
				// TODO implement <dtmf>
				if(!atts) {	// no attributes were specified, it's an error (digits is mandatory)
					message->error(400, "digits");
					return;
				}
				int i = 0;
				string digits = "";
				while(atts[i]) {
					if(!atts[i])
						break;
					if(!strcmp(atts[i], "digits")) {
						// TODO Handle dtmf digits, we ignore it currently...
						cout << "[IVR] *** we ignore type currently, except for checking it (mandatory attribute)..." << endl;
						digits = atts[i+1];
					} else if(!strcmp(atts[i], "level"))
						// TODO Handle dtmf level, we ignore it currently...
						cout << "[IVR] *** we ignore dtmf::level currently..." << endl;
					else if(!strcmp(atts[i], "duration"))
						// TODO Handle dtmf duration, we ignore it currently...
						cout << "[IVR] *** we ignore dtmf::duration currently..." << endl;
					else if(!strcmp(atts[i], "interval"))
						// TODO Handle dtmf interval, we ignore it currently...
						cout << "[IVR] *** we ignore dtmf::interval currently..." << endl;
					else {
						message->error(431, atts[i]);
						return;
					}
					i += 2;
				}
				if(digits == "") {	// no attributes were specified, it's an error (digits is mandatory)
					message->error(400, "digits");
					return;
				}
			} else {
				message->error(431, name);
				return;
			}
		} else {	// Part of <grammar> or something else?
			if(!message->digging) {
				message->error(431, name);
				return;
			} else {	// Every deeper element is domain of <grammar>: save it and parse it later
				// Save the whole subelement ant its attributes and children: we'll parse it later
				message->digging = true;
				stringstream content;
				if(!atts)	// No attributes
					content << "<" << name << ">";
				else {
					int i=0;
					content << "<" << name;
					while(atts[i]) {	// Write down all attributes
						if(!atts[i])
							break;
						content << " " << atts[i] << "=\"" << atts[i+1] << "\"";
						i += 2;
					}
					content << ">";
					
				}
				message->content.append(content.str());
			}
		}
	} else {	// Deeper elements are not allowed (unless we support <grammar>)
		if(!message->digging) {
			message->error(431, name);
			return;
		} else {	// Every deeper element is domain of <grammar>: save it and parse it later
			// Save the whole subelement ant its attributes and children: we'll parse it later
			message->digging = true;
			stringstream content;
			if(!atts)	// No attributes
				content << "<" << name << ">";
			else {
				int i=0;
				content << "<" << name;
				while(atts[i]) {	// Write down all attributes
					if(!atts[i])
						break;
					content << " " << atts[i] << "=\"" << atts[i+1] << "\"";
					i += 2;
				}
				content << ">";
				
			}
			message->content.append(content.str());
		}
	}
}

static void XMLCALL valueElementBI(void *msg, const XML_Char *s, int len)
{
	IvrMessage *message = (IvrMessage *)msg;
	if(message->scanonly)
		return;
	if(message->stop)
		return;
	char value[len+1];
	int i=0;
	int pos = 0;
	while(i < len) {	// FIXME...
		if((s[i] == 0x09) || (s[i] == 0x0a) || (s[i] == 0x0d)) {
			i++;
			continue;
		} else if(s[i] == 0x20) {
			if(pos == 0) {	// Only skip backspaces *before* the content itself
				i++;
				continue;
			}
		}
		memcpy(value+pos, s+i, 1);
		i++;
		pos++;
	}
	if(pos == 0)
		return;
	// Now remove backspaces at the end
	i = pos;
	while(1) {
		i--;
		if(value[i] != 0x20);
			break;
		pos = i;
	}
	value[pos] = '\0';
	if(message->digging) {	// Is this element part of <grammar>?
		message->content.append(value);
		return;
	}
	if(message->childs.back() == "param") {
		if(message->level != 4)		// Element is not where it should be
			return;
		cout << "[IVR] <param> value: " << value << " (ignored)" << endl;
		// TODO Get <param>value</param> value
	}
}

static void XMLCALL endElementBI(void *msg, const char *name)
{
	IvrMessage *message = (IvrMessage *)msg;
	if(message->scanonly)
		return;
	if(message->stop)	// Means that something was wrong...
		return;
	if(message->digging) {	// Needed to rebuild <grammar> (unsupported yet)
		message->content.append("</");
		message->content.append(name);
		message->content.append(">");
		if(!strcmp(name, "grammar"))
			message->digging = false;
	}
	message->level--;
	message->childs.pop_back();
	if(message->level > 0)
		return;
	// We have come back to the root, we're done
	int model = message->dialog->getModel();
	if(model == 0) {	// No collect/prompt/control/record??
		message->error(400, "Missing any prompt/control/collect/record operation");
		return;
	} else if((model & MODEL_CONTROL) && !(model & MODEL_PROMPT)) {
		message->error(400, "control missing prompt");	// FIXME
		return;
	} else if((model & MODEL_RECORD) && (model & MODEL_COLLECT)) {	// FIXME
		message->error(433, "record and collect together not supported");
		return;
	}

	if(message->content != "") {	// There's a <grammar> to parse
		// Check if all in <grammar> is valid: it is in message->content, whether it was inline or externally referenced
		cout << "[IVR] Going to parse <grammar> content: " << message->content << endl;
		IvrMessage *newmsg = new IvrMessage();
		newmsg->blob = message->content;
		newmsg->sender = message->sender;
		// First scan the XML blob for errors...
		newmsg->scanonly = true;
		newmsg->digging = false;
		XML_Parser parser = XML_ParserCreate(NULL);
		XML_SetUserData(parser, newmsg);
		XML_SetElementHandler(parser, startElementGR, endElementGR);
		if (XML_Parse(parser, newmsg->blob.c_str(), newmsg->blob.length(), 1) == XML_STATUS_ERROR) {
			cout << "[IVR]     Error parsing <grammar> content: '"
				<< XML_ErrorString(XML_GetErrorCode(parser))
				<< "' at " << XML_GetCurrentLineNumber(parser) << ":"
				<< XML_GetCurrentColumnNumber(parser) << endl;
			cout << "[IVR]     Broken body was:" << endl << newmsg->blob << "(" << dec << newmsg->blob.length() << ")" << endl;
			message->error(400, "invalid 'grammar' XML body");
			XML_ParserFree(parser);
			return;
		}
		XML_ParserFree(parser);
		// ... and then actually parse and handle it
		newmsg->scanonly = false;
		newmsg->newdialog = message->newdialog;
		newmsg->dialog = message->dialog;
		newmsg->tid = message->tid;
		newmsg->pkg = message->pkg;
		newmsg->content = "";			// We can't have any externally referenced content now
		newmsg->externalContent = false;	// 	""	""	""	""
		parser = XML_ParserCreate(NULL);
		XML_SetUserData(parser, newmsg);
		XML_SetElementHandler(parser, startElementGR, endElementGR);
		XML_SetCharacterDataHandler(parser, valueElementGR);
		XML_Parse(parser, newmsg->blob.c_str(), newmsg->blob.length(), 1);
		XML_ParserFree(parser);
		// Was the parsing ok?
		if(newmsg->stop) {
			message->stop = newmsg->stop;
			delete newmsg;
			return;
		}
		delete newmsg;
	}
}


// eXpat callbacks handle the parsing of <grammar> subelements
static void XMLCALL startElementGR(void *msg, const char *name, const char **atts)
{
	IvrMessage *message = (IvrMessage *)msg;
	if(message->scanonly)
		return;
	if(message->stop)
		return;
	if(message->level == 0) {
		// FIXME do some initialization here? it's the root
	}
	message->level++;
	message->childs.push_back(name);
	if(message->level == 1) {
		if(message->childs.back() != "grammar") {	// grammar, root of SRGS
			message->error(400, name);
			return;
		}
		if(!atts) {	// version was not specified
			message->error(400, "grammar missing required attributes");
			return;
		}
		string version = "", nameSpace = "";
		bool mode = false;
		int i = 0;
		while(atts[i]) {	// We don't care about other attributes
			if(!atts[i])
				break;
			if(!strcmp(atts[i], "version"))
				version = atts[i+1];
			else if(!strcmp(atts[i], "xmlns"))
				nameSpace = atts[i+1];
			else if(!strcmp(atts[i], "xml:lang")) {
				cout << "[IVR] *** Ignoring xml:lang=" << atts[i+1] << "..." << endl;
			} else if(!strcmp(atts[i], "mode")) {
				if(!strcmp(atts[i+1], "dtmf"))
					mode = true;
				else {
					message->error(400, "only mode=dtmf is supported");
					return;
				}
			} else if(!strcmp(atts[i], "root")) {
				cout << "[IVR] *** Ignoring root=" << atts[i+1] << "..." << endl;
				message->dialog->ruleToMatch = atts[i+1];
			} else if(!strcmp(atts[i], "tag-format")) {
				cout << "[IVR] *** Ignoring tag-format=" << atts[i+1] << "..." << endl;
			} else if(!strcmp(atts[i], "xml:base")) {
				cout << "[IVR] *** Ignoring xml:base=" << atts[i+1] << "..." << endl;
			} else if(!strcmp(atts[i], "tag-format")) {
				cout << "[IVR] *** Ignoring xml:lang=" << atts[i+1] << "..." << endl;
			} else if(!strcmp(atts[i], "xmlns:xsi")) {
				cout << "[IVR] *** Ignoring xmlns:xsi=" << atts[i+1] << "..." << endl;
			} else if(!strcmp(atts[i], "xsi:schemaLocation")) {
				cout << "[IVR] *** Ignoring xsi:schemaLocation=" << atts[i+1] << "..." << endl;
			} else {
				message->error(431, atts[i]);
				return;
			}
			i += 2;
		}
		if(version == "") {
			message->error(400, "grammar version");
			return;
		}
		if(version != "1.0") {
			message->error(400, "grammar version is not 1.0");
			return;
		}
		if((nameSpace == "") || (nameSpace != "http://www.w3.org/2001/06/grammar")) {
			message->error(431, "Invalid grammar namespace");
			return;
		}
		if(!mode) {
			message->error(400, "only mode=dtmf is supported");
			return;
		}
	} else if(message->level == 2) {
		if((message->childs.back() != "rule") && message->ruleFound) {
			message->error(400, "meta, metadata and lexicon elements must occur before all rule elements");
			return;
		}
		if(message->childs.back() == "lexicon") {	// No meaning for mode=dtmf: just check if it's correct, but then ignore it
			if(!atts) {	// uri was not specified
				message->error(400, "lexicon missing required attributes");
				return;
			}
			bool ok = false;
			int i = 0;
			while(atts[i]) {	// We don't care about other attributes
				if(!atts[i])
					break;
				if(!strcmp(atts[i], "uri")) {
					if(ok) {
						message->error(400, "lexicon uri already provided");
						return;
					}
					ok = true;
					cout << "[IVR] *** Ignoring lexicon uri=" << atts[i+1] << "..." << endl;
				} else if(!strcmp(atts[i], "type")) {
					cout << "[IVR] *** Ignoring lexicon type=" << atts[i+1] << "..." << endl;
				} else {
					message->error(431, atts[i]);
					return;
				}
				i += 2;
			}
			if(!ok) {
				message->error(400, "lexicon uri missing");
				return;
			}
			return;
		} else if(message->childs.back() == "meta") {	// No meaning for mode=dtmf: just check if it's correct, but then ignore it
			if(!atts) {	// content was not specified
				message->error(400, "meta missing required attributes");
				return;
			}
			bool ok = false;
			int i = 0;
			while(atts[i]) {	// We don't care about other attributes
				if(!atts[i])
					break;
				if(!strcmp(atts[i], "name")) {
					message->metaName = true;
					cout << "[IVR] *** Ignoring meta name=" << atts[i+1] << "..." << endl;
				} else if(!strcmp(atts[i], "http-equiv")) {
					message->metaHttp = true;
					cout << "[IVR] *** Ignoring meta http-equiv=" << atts[i+1] << "..." << endl;
				} else if(!strcmp(atts[i], "content")) {
					ok = true;
					cout << "[IVR] *** Ignoring meta content=" << atts[i+1] << "..." << endl;
				} else {
					message->error(431, atts[i]);
					return;
				}
				i += 2;
			}
			if(!ok) {
				message->error(400, "meta content missing");
				return;
			}
			if(message->metaName && message->metaHttp) {
				message->error(400, "can't have both name AND http-equiv in grammar meta");
				return;
			}
			return;
		} else if(message->childs.back() == "metadata") {	// No meaning for mode=dtmf: just check if it's correct, but then ignore it
			if(!atts) {
				message->metaData = true;
				return;
			}
			int i = 0;
			while(atts[i]) {	// We don't care about any attribute (there shouldn't be)
				if(!atts[i])
					break;
				message->error(431, atts[i]);
				return;
			}
			message->metaData = true;
			return;
		} else if(message->childs.back() == "tag") {	// No meaning for mode=dtmf: just check if it's correct, but then ignore it
			if(!atts)
				return;
			int i = 0;
			while(atts[i]) {	// We don't care about any attribute (there shouldn't be)
				if(!atts[i])
					break;
				message->error(431, atts[i]);
				return;
			}
			return;
		} else if(message->childs.back() == "rule") {	// <rule>
			cout << "[IVR] TODO: rule" << endl;
			message->ruleFound = true;	// Now we're cooking...
			// TODO
			string id = "", scope = "";
			bool privateScope = true;
			int i = 0;
			while(atts[i]) {	// We don't care about other attributes
				if(!atts[i])
					break;
				if(!strcmp(atts[i], "id")) {
					if(id != "") {
						message->error(400, "rule id already provided");
						return;
					}
					id = atts[i+1];
				} else if(!strcmp(atts[i], "scope")) {
					if(scope != "") {
						message->error(400, "rule scope already provided");
						return;
					}
					if(!strcmp(atts[i+1], "private"))
						privateScope = true;
					else if(!strcmp(atts[i+1], "public"))
						privateScope = false;
					else {
						message->error(400, "invalid rule scope");
						return;
					}
					scope = atts[i+1];
				} else {
					message->error(431, atts[i]);
					return;
				}
				i += 2;
			}
			// TODO Check if id is unique...
			if(message->dialog->srgsRules[id] != NULL) {
				message->error(400, "duplicate rule");
				return;
			}
			SrgsRule *rule = new SrgsRule(id, privateScope);
			message->dialog->currentStep = 0;
			cout << "[IVR] New rule: " << rule->id << " (scope=" << (rule->privateScope ? "private" : "public") << ")" << endl;
			if(privateScope)
				message->dialog->currentRule = rule->id;
			else
				message->dialog->currentRule = "";
		}
	} else if(message->level == 3) {
		message->childs.pop_back();
		string owner = message->childs.back();
		message->childs.push_back(name);
		if(message->metaData) {
			if(!atts)
				return;
			int i = 0;
			while(atts[i]) {	// We don't care about other attributes
				if(!atts[i])
					break;
				cout << "[IVR] Ignoring metadata " << name << "(L=" << dec << message->level << "):" << atts[i] << "=" << atts[i+1] << endl;
				i += 2;
				return;
			}
			return;
		} else if(owner != "rule") {	// only <rule> is allowed any child at this level
			message->error(431, name);
			return;
		}
		if(message->childs.back() == "one-of") {
			message->dialog->oneOf = true;
			cout << "[IVR] TODO: one-of" << endl;
			// TODO
			if(!atts)
				return;
			int i = 0;
			while(atts[i]) {	// We don't care about any attribute (there shouldn't be)
				if(!atts[i])
					break;
				message->error(431, atts[i]);
				return;
			}
			return;
		} else if(message->childs.back() == "ruleref") {
			cout << "[IVR] TODO: ruleref (L3)" << endl;
			// TODO
			string uri = "", type = "", special = "";
			int i = 0;
			while(atts[i]) {	// We don't care about other attributes
				if(!atts[i])
					break;
				if(!strcmp(atts[i], "uri")) {
					if(uri != "") {
						message->error(400, "ruleref uri already provided");
						return;
					} else if(special != "") {
						message->error(400, "ruleref can't have both uri AND special");
						return;
					}
					uri = atts[i+1];
				} else if(!strcmp(atts[i], "special")) {
					if(special != "") {
						message->error(400, "ruleref special already provided");
						return;
					} else if(uri != "") {
						message->error(400, "ruleref can't have both uri AND special");
						return;
					}
					special = atts[i+1];
				} else if(!strcmp(atts[i], "type")) {
					if(type != "") {
						message->error(400, "ruleref type already provided");
						return;
					}
					type = atts[i+1];
				} else {
					message->error(431, atts[i]);
					return;
				}
				i += 2;
			}
			if((uri == "") && (special == "")) {
				message->error(400, "ruleref missing uri/special");
				return;
			}
			if((uri == "NULL") || (uri == "VOID") || (uri == "GARBAGE")) {
				message->error(400, "invalid ruleref uri (reserved)");
				return;
			}
			if((special != "") && (special != "NULL") && (special != "VOID") && (special != "GARBAGE")) {
				message->error(400, "invalid ruleref special");
				return;
			}
			// TODO Check uri and type...
			cout << "[IVR] New ruleref: ";
			if(uri != "")
				cout << "uri=" << uri;
			else if(special != "")
				cout << "special=" << special;
			if(type != "")
				cout << " (" << type << ")";
			cout << endl;
		} else if(message->childs.back() == "example") {
			cout << "[IVR] TODO: example" << endl;
			// TODO
		} else {
			message->error(431, name);
			return;
		}
	} else if(message->level == 4) {
		if(message->metaData) {
			if(!atts)
				return;
			int i = 0;
			while(atts[i]) {	// We don't care about other attributes
				if(!atts[i])
					break;
				cout << "[IVR] Ignoring metadata " << name << "(L=" << dec << message->level << "):" << atts[i] << "=" << atts[i+1] << endl;
				i += 2;
				return;
			}
			return;
		}
		if(message->childs.back() == "item") {
			cout << "[IVR] TODO: item (L4)" << endl;
			// TODO
			string repeat = "", repeatProb = "", weight = "";
			int i = 0;
			while(atts[i]) {	// We don't care about other attributes
				if(!atts[i])
					break;
				if(!strcmp(atts[i], "repeat")) {
					if(repeat != "") {
						message->error(400, "item repeat already provided");
						return;
					}
					repeat = atts[i+1];
				} else if(!strcmp(atts[i], "weight")) {
					if(weight != "") {
						message->error(400, "item weight already provided");
						return;
					}
					weight = atts[i+1];
				} else if(!strcmp(atts[i], "repeat-prob")) {
					if(repeatProb != "") {
						message->error(400, "item repeat-prob already provided");
						return;
					}
					repeatProb = atts[i+1];
				} else {
					message->error(431, atts[i]);
					return;
				}
				i += 2;
			}
			// TODO Check repeat and weight...
			cout << "[IVR] New item: " << endl;
			if((repeat == "") && (repeatProb != "")) {
				message->error(400, "repeat-prob with no repeat");
				return;
			}
			if(repeat != "") {
				cout << "[IVR] \trepeat=" << repeat << endl;
				// First of all try a direct match
				bool ok = true;
				uint16_t n = positiveInteger(repeat, &ok);
				if(ok)
					cout << "[IVR] \t\tn=" << dec << n << endl;
				else {
					// Now check if it's a range (m-n)
					regex re;
					cmatch matches;
					re.assign("(\\d+)\\-(\\d+)?", regex_constants::icase);
					if(!regex_match(repeat.c_str(), matches, re)) {
						message->error(400, "Invalid repeat for item");
						return;
					}
					uint16_t m = 0;
					if(matches[1].first == matches[1].second)
						m = 0;
					else {
						string match(matches[1].first, matches[1].second);
						m = atoi(match.c_str());
					}
					n = 0;
					if(matches[2].first == matches[2].second)
						n = USHRT_MAX;
					else {
						string match(matches[2].first, matches[2].second);
						n = atoi(match.c_str());
					}
					cout << "[IVR] \t\tm=" << dec << m << " --> n=" << dec << n << endl;
				}
			}
			if(repeatProb != "")
				cout << "[IVR] \trepeat-prob=" << repeatProb << " (ignored)" << endl;
			if(weight != "")
				cout << "[IVR] \tweight=" << weight << " (ignored)" << endl;
		} else if(message->childs.back() == "ruleref") {
			cout << "[IVR] TODO: ruleref (L4)" << endl;
			// TODO
			string uri = "", type = "", special = "";
			int i = 0;
			while(atts[i]) {	// We don't care about other attributes
				if(!atts[i])
					break;
				if(!strcmp(atts[i], "uri")) {
					if(uri != "") {
						message->error(400, "ruleref uri already provided");
						return;
					} else if(special != "") {
						message->error(400, "ruleref can't have both uri AND special");
						return;
					}
					uri = atts[i+1];
				} else if(!strcmp(atts[i], "special")) {
					if(special != "") {
						message->error(400, "ruleref special already provided");
						return;
					} else if(uri != "") {
						message->error(400, "ruleref can't have both uri AND special");
						return;
					}
					special = atts[i+1];
				} else if(!strcmp(atts[i], "type")) {
					if(type != "") {
						message->error(400, "ruleref type already provided");
						return;
					}
					type = atts[i+1];
				} else {
					message->error(431, atts[i]);
					return;
				}
				i += 2;
			}
			if((uri == "") && (special == "")) {
				message->error(400, "ruleref missing uri/special");
				return;
			}
			if((uri == "NULL") || (uri == "VOID") || (uri == "GARBAGE")) {
				message->error(400, "invalid ruleref uri (reserved)");
				return;
			}
			if((special != "") && (special != "NULL") && (special != "VOID") && (special != "GARBAGE")) {
				message->error(400, "invalid ruleref special");
				return;
			}
			// TODO Check uri and type...
			cout << "[IVR] New ruleref: ";
			if(uri != "")
				cout << "uri=" << uri;
			else if(special != "")
				cout << "special=" << special;
			if(type != "")
				cout << " (" << type << ")";
			cout << endl;
		} else {
			message->error(431, name);
			return;
		}
	} else if(message->level == 5) {
		if(message->metaData) {
			if(!atts)
				return;
			int i = 0;
			while(atts[i]) {	// We don't care about other attributes
				if(!atts[i])
					break;
				cout << "[IVR] Ignoring metadata " << name << "(L=" << dec << message->level << "):" << atts[i] << "=" << atts[i+1] << endl;
				i += 2;
				return;
			}
			return;
		}
		if(message->childs.back() == "item") {
			cout << "[IVR] TODO: item (L5)" << endl;
			// TODO
			string repeat = "", repeatProb = "", weight = "";
			int i = 0;
			while(atts[i]) {	// We don't care about other attributes
				if(!atts[i])
					break;
				if(!strcmp(atts[i], "repeat")) {
					if(repeat != "") {
						message->error(400, "item repeat already provided");
						return;
					}
					repeat = atts[i+1];
				} else if(!strcmp(atts[i], "weight")) {
					if(weight != "") {
						message->error(400, "item weight already provided");
						return;
					}
					weight = atts[i+1];
				} else if(!strcmp(atts[i], "repeat-prob")) {
					if(repeatProb != "") {
						message->error(400, "item repeat-prob already provided");
						return;
					}
					repeatProb = atts[i+1];
				} else {
					message->error(431, atts[i]);
					return;
				}
				i += 2;
			}
			// TODO Check repeat and weight...
			cout << "[IVR] New item: " << endl;
			if((repeat == "") && (repeatProb != "")) {
				message->error(400, "repeat-prob with no repeat");
				return;
			}
			if(repeat != "") {
				cout << "[IVR] \trepeat=" << repeat << endl;
				// First of all try a direct match
				bool ok = true;
				uint16_t n = positiveInteger(repeat, &ok);
				if(ok)
					cout << "[IVR] \t\tn=" << dec << n << endl;
				else {
					// Now check if it's a range (m-n)
					regex re;
					cmatch matches;
					re.assign("(\\d+)\\-(\\d+)?", regex_constants::icase);
					if(!regex_match(repeat.c_str(), matches, re)) {
						message->error(400, "Invalid repeat for item");
						return;
					}
					uint16_t m = 0;
					if(matches[1].first == matches[1].second)
						m = 0;
					else {
						string match(matches[1].first, matches[1].second);
						m = atoi(match.c_str());
					}
					n = 0;
					if(matches[2].first == matches[2].second)
						n = USHRT_MAX;
					else {
						string match(matches[2].first, matches[2].second);
						n = atoi(match.c_str());
					}
					cout << "[IVR] \t\tm=" << dec << m << " --> n=" << dec << n << endl;
				}
			}
			if(repeatProb != "")
				cout << "[IVR] \trepeat-prob=" << repeatProb << " (ignored)" << endl;
			if(weight != "")
				cout << "[IVR] \tweight=" << weight << " (ignored)" << endl;
		} else if(message->childs.back() == "ruleref") {
			cout << "[IVR] TODO: ruleref (L5)" << endl;
			// TODO
			string uri = "", type = "", special = "";
			int i = 0;
			while(atts[i]) {	// We don't care about other attributes
				if(!atts[i])
					break;
				if(!strcmp(atts[i], "uri")) {
					if(uri != "") {
						message->error(400, "ruleref uri already provided");
						return;
					} else if(special != "") {
						message->error(400, "ruleref can't have both uri AND special");
						return;
					}
					uri = atts[i+1];
				} else if(!strcmp(atts[i], "special")) {
					if(special != "") {
						message->error(400, "ruleref special already provided");
						return;
					} else if(uri != "") {
						message->error(400, "ruleref can't have both uri AND special");
						return;
					}
					special = atts[i+1];
				} else if(!strcmp(atts[i], "type")) {
					if(type != "") {
						message->error(400, "ruleref type already provided");
						return;
					}
					type = atts[i+1];
				} else {
					message->error(431, atts[i]);
					return;
				}
				i += 2;
			}
			if((uri == "") && (special == "")) {
				message->error(400, "ruleref missing uri/special");
				return;
			}
			if((uri == "NULL") || (uri == "VOID") || (uri == "GARBAGE")) {
				message->error(400, "invalid ruleref uri (reserved)");
				return;
			}
			if((special != "") && (special != "NULL") && (special != "VOID") && (special != "GARBAGE")) {
				message->error(400, "invalid ruleref special");
				return;
			}
			// TODO Check uri and type...
			cout << "[IVR] New ruleref: ";
			if(uri != "")
				cout << "uri=" << uri;
			else if(special != "")
				cout << "special=" << special;
			if(type != "")
				cout << " (" << type << ")";
			cout << endl;
		} else {
			message->error(431, name);
			return;
		}
	} else if(message->level == 6) {
		if(message->metaData) {
			if(!atts)
				return;
			int i = 0;
			while(atts[i]) {	// We don't care about other attributes
				if(!atts[i])
					break;
				cout << "[IVR] Ignoring metadata " << name << "(L=" << dec << message->level << "):" << atts[i] << "=" << atts[i+1] << endl;
				i += 2;
				return;
			}
			return;
		}
		if(message->childs.back() == "ruleref") {
			cout << "[IVR] TODO: ruleref (L6)" << endl;
			// TODO
			string uri = "", type = "", special = "";
			int i = 0;
			while(atts[i]) {	// We don't care about other attributes
				if(!atts[i])
					break;
				if(!strcmp(atts[i], "uri")) {
					if(uri != "") {
						message->error(400, "ruleref uri already provided");
						return;
					} else if(special != "") {
						message->error(400, "ruleref can't have both uri AND special");
						return;
					}
					uri = atts[i+1];
				} else if(!strcmp(atts[i], "special")) {
					if(special != "") {
						message->error(400, "ruleref special already provided");
						return;
					} else if(uri != "") {
						message->error(400, "ruleref can't have both uri AND special");
						return;
					}
					special = atts[i+1];
				} else if(!strcmp(atts[i], "type")) {
					if(type != "") {
						message->error(400, "ruleref type already provided");
						return;
					}
					type = atts[i+1];
				} else {
					message->error(431, atts[i]);
					return;
				}
				i += 2;
			}
			if((uri == "") && (special == "")) {
				message->error(400, "ruleref missing uri/special");
				return;
			}
			if((uri == "NULL") || (uri == "VOID") || (uri == "GARBAGE")) {
				message->error(400, "invalid ruleref uri (reserved)");
				return;
			}
			if((special != "") && (special != "NULL") && (special != "VOID") && (special != "GARBAGE")) {
				message->error(400, "invalid ruleref special");
				return;
			}
			// TODO Check uri and type...
			cout << "[IVR] New ruleref: ";
			if(uri != "")
				cout << "uri=" << uri;
			else if(special != "")
				cout << "special=" << special;
			if(type != "")
				cout << " (" << type << ")";
			cout << endl;
		} else {
			message->error(431, name);
			return;
		}
	} else {	// Deeper elements are not allowed (unless we're digging <metadata> just for being compliant)
		if(message->metaData) {
			if(!atts)
				return;
			int i = 0;
			while(atts[i]) {	// We don't care about other attributes
				if(!atts[i])
					break;
				cout << "[IVR] Ignoring metadata " << name << "(L=" << dec << message->level << "):" << atts[i] << "=" << atts[i+1] << endl;
				i += 2;
				return;
			}
			return;
		} else {
			message->error(431, name);
			return;
		}
	}

}

static void XMLCALL valueElementGR(void *msg, const XML_Char *s, int len)
{
	IvrMessage *message = (IvrMessage *)msg;
	if(message->scanonly)
		return;
	if(message->stop)
		return;
	char value[len+1];
	int i=0;
	int pos = 0;
	while(i < len) {	// FIXME...
		if((s[i] == 0x09) || (s[i] == 0x0a) || (s[i] == 0x0d)) {
			i++;
			continue;
		} else if(s[i] == 0x20) {
			if((pos == 0) ||
				(message->childs.back() == "item") ||
				(message->childs.back() == "rule")) {	// Only skip backspaces *before* the content itself
					i++;
					continue;
			}
		}
		memcpy(value+pos, s+i, 1);
		i++;
		pos++;
	}
	if(pos == 0)
		return;
	// Now remove backspaces at the end
	i = pos;
	while(1) {
		i--;
		if(value[i] != 0x20);
			break;
		pos = i;
	}
	value[pos] = '\0';
	if(message->metaData) {
		cout << "[IVR] Ignoring metadata value " << message->childs.back() << "(L=" << dec << message->level << "):" << value << endl;
		return;
	}
	if(message->childs.back() == "tag") {
		cout << "[IVR] *** Ignoring tag=" << value << endl;
		return;
	} else if(message->childs.back() == "example") {
		cout << "[IVR] *** Ignoring example=" << value << endl;
		return;
	} else if((message->childs.back() == "item") || (message->childs.back() == "rule")) {
		bool ok = true;
		int i=0;
		SrgsRule *rule = NULL;
		SrgsAlternative *alternative = NULL;
		if(message->dialog->currentRule != "") {	// Add item/rule to the list
			rule = message->dialog->srgsRules[message->dialog->currentRule];
			if(rule != NULL) {
				if((message->dialog->currentStep == 0) && rule->alternatives.empty()) {
					alternative = new SrgsAlternative();
					rule->alternatives.push_back(alternative);
				} else {
					int i=0;
					list<SrgsAlternative*>::iterator iter;
					for(iter = rule->alternatives.begin(); iter != rule->alternatives.end(); iter++) {
						if(i < message->dialog->currentStep) {
							i++;
							continue;
						}
						alternative = (*iter);
						break;
					}
				}
			}
		}
		cout << "[IVR] Validating <" << message->childs.back() << ">: ";
		while(1) {
			if(i > 0)
				cout << " | ";
			cout << value[i];
			stringstream tmp;
			tmp << value[i];
			int tone = dtmfDigit(tmp.str(), &ok);
			if(!ok) {
				cout << " <--!!!" << endl;
				message->error(400, "Invalid DTMF code for item");
				return;
			}
			i++;
			if(i == strlen(value))
				break;
		}
		if(alternative != NULL) {
			alternative->digits.push_back(value);
			if(!message->dialog->oneOf)	// End this alternative and create a new step
				rule->alternatives.push_back(new SrgsAlternative());
		}	
		cout << endl << value << endl;
		return;
	}
	// TODO
}

static void XMLCALL endElementGR(void *msg, const char *name)
{
	IvrMessage *message = (IvrMessage *)msg;
	if(message->scanonly)
		return;
	if(message->stop)	// Means that something was wrong...
		return;
	if(!strcmp(name, "metadata"))
		message->metaData = false;
	else if(!strcmp(name, "rule")) {
		message->dialog->currentRule = "";
		message->dialog->currentStep = 0;
	} else if(!strcmp(name, "one-of"))
		message->dialog->oneOf = false;
	message->level--;
	message->childs.pop_back();
	if(message->level > 0)
		return;
	// We have come back to the root, we're done
}
