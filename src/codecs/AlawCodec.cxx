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
 * \brief A-law Audio Codec (based on http://hazelware.luggle.com/tutorials/mulawcompression.html)
 *
 * \author Lorenzo Miniero <lorenzo.miniero@unina.it>
 *
 * \ingroup codecs
 * \ref codecs
 */

#include "MediaCtrlCodec.h"

using namespace std;
using namespace mediactrl;


#define ALAW_FRAME_LENGTH	160
#define RAW_BUFFER_LENGTH	8096


/// A-law codec
/**
* @class AlawCodec AlawCodec.cxx
* The class, inheriting from MediaCtrlCodec, which implements the A-law encoding and decoding features.
*/
class AlawCodec : public MediaCtrlCodec {
	public:
		/**
		* @fn AlawCodec()
		* Constructor. Just fills in the default stuff.
		* @note The codec is actually initialized only after a call to the start() member
		*/
		AlawCodec();
		/**
		* @fn ~AlawCodec()
		* Destructor.
		*/
		~AlawCodec();

		void setCollector(void *frameCollector);

		/**
		* @fn checkAvt(int avt)
		* This method checks if the provided AVT profile identifier is valid for this codec.
		* @param avt A numeric AVT profile identifier
		* @returns true if it is, false otherwise
		*/
		bool checkAvt(int avt);

		/**
		* @fn addSetting(string var, string value)
		* Method to pass to the codec some specific settings before startup (unused)
		* @param var The name of the setting (ignored)
		* @param value The value(s) for the setting (ignored)
		*/
		void addSetting(string var, string value) { return; };

		/**
		* @fn encode(MediaCtrlFrame *outgoing)
		* Encodes a raw frame to A-law.
		* @param outgoing A raw frame
		* @returns An A-law encoded MediaCtrlFrame, if successful, NULL otherwise
		*/
		MediaCtrlFrame *encode(MediaCtrlFrame *outgoing);
		/**
		* @fn decode(MediaCtrlFrame *incoming)
		* Decodes an A-law encoded frame to raw.
		* @param incoming The A-law encoded frame
		* @returns The raw MediaCtrlFrame decoded frame, if successful, NULL otherwise
		*/
		MediaCtrlFrame *decode(MediaCtrlFrame *incoming);

		/**
		* @fn start()
		* This method actually initializes the codec functionality, making it ready to be used.
		*/
		bool start();
};


// AlawCodec Class Factories
extern "C" MediaCtrlCodec* create()
{
	AlawCodec *codec = new AlawCodec();
	return codec;
}

extern "C" void destroy(MediaCtrlCodec* c)
{
	delete c;
}

extern "C" void purge()
{
	// Nothing to do here
}


// Constants, tables and helpers
const int cBias = 0x84;
const int cClip = 32635;

static char ALawCompressTable[128] =
{
	1,1,2,2,3,3,3,3,
	4,4,4,4,4,4,4,4,
	5,5,5,5,5,5,5,5,
	5,5,5,5,5,5,5,5,
	6,6,6,6,6,6,6,6,
	6,6,6,6,6,6,6,6,
	6,6,6,6,6,6,6,6,
	6,6,6,6,6,6,6,6,
	7,7,7,7,7,7,7,7,
	7,7,7,7,7,7,7,7,
	7,7,7,7,7,7,7,7,
	7,7,7,7,7,7,7,7,
	7,7,7,7,7,7,7,7,
	7,7,7,7,7,7,7,7,
	7,7,7,7,7,7,7,7,
	7,7,7,7,7,7,7,7
};

static short ALawDecompressTable[256] =
{
     -5504, -5248, -6016, -5760, -4480, -4224, -4992, -4736,
     -7552, -7296, -8064, -7808, -6528, -6272, -7040, -6784,
     -2752, -2624, -3008, -2880, -2240, -2112, -2496, -2368,
     -3776, -3648, -4032, -3904, -3264, -3136, -3520, -3392,
     -22016,-20992,-24064,-23040,-17920,-16896,-19968,-18944,
     -30208,-29184,-32256,-31232,-26112,-25088,-28160,-27136,
     -11008,-10496,-12032,-11520,-8960, -8448, -9984, -9472,
     -15104,-14592,-16128,-15616,-13056,-12544,-14080,-13568,
     -344,  -328,  -376,  -360,  -280,  -264,  -312,  -296,
     -472,  -456,  -504,  -488,  -408,  -392,  -440,  -424,
     -88,   -72,   -120,  -104,  -24,   -8,    -56,   -40,
     -216,  -200,  -248,  -232,  -152,  -136,  -184,  -168,
     -1376, -1312, -1504, -1440, -1120, -1056, -1248, -1184,
     -1888, -1824, -2016, -1952, -1632, -1568, -1760, -1696,
     -688,  -656,  -752,  -720,  -560,  -528,  -624,  -592,
     -944,  -912,  -1008, -976,  -816,  -784,  -880,  -848,
      5504,  5248,  6016,  5760,  4480,  4224,  4992,  4736,
      7552,  7296,  8064,  7808,  6528,  6272,  7040,  6784,
      2752,  2624,  3008,  2880,  2240,  2112,  2496,  2368,
      3776,  3648,  4032,  3904,  3264,  3136,  3520,  3392,
      22016, 20992, 24064, 23040, 17920, 16896, 19968, 18944,
      30208, 29184, 32256, 31232, 26112, 25088, 28160, 27136,
      11008, 10496, 12032, 11520, 8960,  8448,  9984,  9472,
      15104, 14592, 16128, 15616, 13056, 12544, 14080, 13568,
      344,   328,   376,   360,   280,   264,   312,   296,
      472,   456,   504,   488,   408,   392,   440,   424,
      88,    72,   120,   104,    24,     8,    56,    40,
      216,   200,   248,   232,   152,   136,   184,   168,
      1376,  1312,  1504,  1440,  1120,  1056,  1248,  1184,
      1888,  1824,  2016,  1952,  1632,  1568,  1760,  1696,
      688,   656,   752,   720,   560,   528,   624,   592,
      944,   912,  1008,   976,   816,   784,   880,   848
};

uint8_t LinearToALawSample(short sample)
{
	int sign;
	int exponent;
	int mantissa;
	uint8_t compressedByte;

	sign = ((~sample) >> 8) & 0x80;
	if (!sign)
		sample = (short)-sample;
	if (sample > cClip)
		sample = cClip;
	if (sample >= 256) {
		exponent = (int)ALawCompressTable[(sample >> 8) & 0x7F];
		mantissa = (sample >> (exponent + 3) ) & 0x0F;
		compressedByte = ((exponent << 4) | mantissa);
	} else
		compressedByte = (uint8_t)(sample >> 4);

	compressedByte ^= (sign ^ 0x55);
	return compressedByte;
}


// Class Methods
AlawCodec::AlawCodec()
{
	MCMINIT();
	cout << "[ALAW] Creating AlawCodec()" << endl;
	media = MEDIACTRL_MEDIA_AUDIO;
	format = MEDIACTRL_CODEC_ALAW;
	clockrate = 8000;
	name = nameMask = "PCMA";
	blockLen = ALAW_FRAME_LENGTH;
	avts.push_back(8);	// 0 is the static AVT payload format for A-law
	started = false;
}

AlawCodec::~AlawCodec()
{
	cout << "[ALAW] Removing AlawCodec()" << endl;
}

void AlawCodec::setCollector(void *frameCollector)
{
	setCommonCollector(frameCollector);
	cout << "[ALAW] Frame Collector " << (getCollector() ? "OK" : "NOT OK :(") << endl;
}

bool AlawCodec::start()
{
	started = true;
	// Nothing to be done here...
	return true;
}

bool AlawCodec::checkAvt(int avt)
{
	return(avt == avts.front());
}

MediaCtrlFrame *AlawCodec::encode(MediaCtrlFrame *outgoing)
{
	// FIXME are all these checks really useful?
	if(outgoing == NULL)
		return NULL;
	if((outgoing->getBuffer() == NULL) || (outgoing->getLen() < 0))
		return NULL;

	uint8_t *buffer = (uint8_t *)MCMALLOC(ALAW_FRAME_LENGTH, sizeof(uint8_t)), *tmp = buffer;
	if(!buffer)
		return NULL;

	int i=0;
	short *samples = (short*)outgoing->getBuffer();
	for(i = 0; i < ALAW_FRAME_LENGTH; i++)
		*tmp++ = LinearToALawSample(*samples++);

	MediaCtrlFrame *encoded = new MediaCtrlFrame();		// An U-law Frame
	encoded->setAllocator(CODEC);
	encoded->setFormat(MEDIACTRL_CODEC_ALAW);
	encoded->setBuffer((uint8_t *)buffer, ALAW_FRAME_LENGTH);
	MCMFREE(buffer);

	return encoded;
}

MediaCtrlFrame *AlawCodec::decode(MediaCtrlFrame *incoming)
{
	// FIXME are all these checks really useful?
	if(incoming == NULL)
		return NULL;
	if((incoming->getBuffer() == NULL) || (incoming->getLen() != ALAW_FRAME_LENGTH))
		return NULL;

	uint8_t *samples = incoming->getBuffer();
	short *buffer = (short *)MCMALLOC(ALAW_FRAME_LENGTH, sizeof(short)), *tmp = buffer;
	if(!buffer)
		return NULL;
	int i=0;
	for(i = 0; i < ALAW_FRAME_LENGTH; i++)
		*tmp++ = ALawDecompressTable[*samples++];

	MediaCtrlFrame *decoded = new MediaCtrlFrame();		// A raw frame
	decoded->setAllocator(CODEC);
	decoded->setBuffer((uint8_t *)buffer, ALAW_FRAME_LENGTH*2);
	MCMFREE(buffer);

	return decoded;
}
