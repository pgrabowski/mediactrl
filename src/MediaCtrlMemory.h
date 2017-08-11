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

#ifndef _MEDIA_CTRL_MEMORY_H
#define _MEDIA_CTRL_MEMORY_H

/*! \file
 *
 * \brief Memory Management Header (to GC or not to GC?)
 *
 * \author Lorenzo Miniero <lorenzo.miniero@unina.it>
 *
 * \ingroup utils
 * \ref utils
 */

#include <iostream>

#ifdef USE_GC

// Use GC Garbage Collector
#define GC_PTHREADS
//#define _REENTRANT
#include "gc.h"
#include "gc_cpp.h"
#define MCMALLOC(x,y)	GC_MALLOC(x)
#define MCMREALLOC(x,y)	GC_REALLOC(x,y)
#define MCMFREE(x)		if(x) x=NULL
#define MCMINIT()	\
	cout << "Using GC Garbage Collector" << endl;	\
	GC_INIT();

#else

// Use calloc/free/etc
#define MCMALLOC(x,y)	calloc(x,y)
#define MCMREALLOC(x,y)	realloc(x,y)
#define MCMFREE(x)	\
		free(x);	\
		x = NULL;
#define MCMINIT()	\
	cout << "Using calloc/realloc/free" << endl;

// Dummy class, needed for 'public gc' inheritance (GC uses it)
class gc {
	public:
		gc() {};
		~gc() {};
};

#endif

#endif
