/*
Copyright (c) 2017, Richard Eakin - All rights reserved.

Redistribution and use in source and binary forms, with or without modification, are permitted provided
that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this list of conditions and
the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and
the following disclaimer in the documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED
WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.
*/

#pragma once

#include "mason/Export.h"

#include "cinder/Color.h"
#include "cinder/Surface.h"

#include <vector>

namespace mason {

class MA_API ColorLUT {
  public:

	struct Stop {
		Stop( float percent, const ci::Colorf &color )
			: percent( percent ), color( color )
		{}

		float		percent = 0;
		ci::Colorf	color = ci::Colorf::black();
	};

	ColorLUT() = default;
	ColorLUT( size_t size, const std::vector<Stop> &stops );
	ColorLUT( const ci::ImageSourceRef &imageSource );

	const ci::Colorf& lookup( float f );

	ci::Surface32f	makeSurface32f();

  private:
	void sortStops();

	void fillTable();

	std::vector<Stop>		mStops;
	std::vector<ci::Colorf>	mLUT;
};

} // namespace mason
