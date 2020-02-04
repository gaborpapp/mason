/*
 Copyright (c) 2015, Richard Eakin - All rights reserved.

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

#include "mason/audio/CompressorNode.h"
#include "cinder/CinderAssert.h"
#include "cinder/CinderMath.h"
#include "cinder/Log.h"

#include "cinder/audio/Context.h"

using namespace ci;
using namespace ci::audio;

namespace mason { namespace audio {

const size_t MaxPreDelayFrames = 1024;
const size_t MaxPreDelayFramesMask = MaxPreDelayFrames - 1;
const size_t DefaultPreDelayFrames = 256;

float decibelsToLinear( float decibels )
{
	return powf( 10, 0.05f * decibels );
}

float linearToDecibels( float linear )
{
	// It's not possible to calculate decibels for a zero linear value since it would be -Inf.
	// -1000.0 dB represents a very tiny linear value in case we ever reach this case.
	CI_ASSERT( linear );

	return 20 * log10f( linear );
}

double discreteTimeConstantForSampleRate(double timeConstant, double sampleRate)
{
	return 1 - exp(-1 / (sampleRate * timeConstant));
}

CompressorNode::CompressorNode( const Format &format )
	: Node( format ),
	mParamThreshold( this, -24 ),
	mParamRatio( this, 12 ),
	mParamKnee( this, 30 ),
	mParamAttackTime( this, 0.003f ),
	mParamReleaseTime( this, 0.25f )
{
}

void CompressorNode::initialize()
{
	m_lastPreDelayFrames = DefaultPreDelayFrames;
	m_preDelayReadIndex = 0;
	m_preDelayWriteIndex = DefaultPreDelayFrames;

	m_ratio = -1;
	m_slope = -1;
	m_linearThreshold = -1;
	m_dbThreshold = -1;
	m_dbKnee = -1;
	m_kneeThreshold = -1;
	m_kneeThresholdDb = -1;
	m_ykneeThresholdDb = -1;
	m_K = -1;

	mPreDelayBuffer.setSize( MaxPreDelayFrames, getNumChannels() );

	reset();

	const float meteringReleaseTimeConstant = 0.325f;
	m_meteringReleaseK = static_cast<float>( discreteTimeConstantForSampleRate( meteringReleaseTimeConstant, getSampleRate() ) );
}

void CompressorNode::reset()
{
	m_detectorAverage = 0;
	m_compressorGain = 1;
	m_meteringGain = 1;

	// Predelay section.
	mPreDelayBuffer.zero();

	m_preDelayReadIndex = 0;
	m_preDelayWriteIndex = DefaultPreDelayFrames;

	m_maxAttackCompressionDiffDb = -1; // uninitialized state
}

// Re-configure look-ahead section pre-delay if delay time has changed.
void CompressorNode::setPreDelayTime( float preDelayTime )
{
	unsigned preDelayFrames = preDelayTime * getSampleRate();
	if( preDelayFrames > MaxPreDelayFrames - 1 )
		preDelayFrames = MaxPreDelayFrames - 1;

	if( m_lastPreDelayFrames != preDelayFrames ) {
		m_lastPreDelayFrames = preDelayFrames;
		mPreDelayBuffer.zero();

		m_preDelayReadIndex = 0;
		m_preDelayWriteIndex = preDelayFrames;
	}
}

// Exponential curve for the knee.
// It is 1st derivative matched at m_linearThreshold and asymptotically approaches the value m_linearThreshold + 1 / k.
float CompressorNode::kneeCurve( float x, float k )
{
	// Linear up to threshold.
	if( x < m_linearThreshold )
		return x;

	return m_linearThreshold + ( 1 - expf( -k * ( x - m_linearThreshold ) ) ) / k;
}

// Full compression curve with constant ratio after knee.
float CompressorNode::saturate( float x, float k )
{
	float y;

	if( x < m_kneeThreshold )
		y = kneeCurve( x, k );
	else {
		// Constant ratio after knee.
		float xDb = linearToDecibels( x );
		float yDb = m_ykneeThresholdDb + m_slope * ( xDb - m_kneeThresholdDb );

		y = decibelsToLinear(yDb);
	}

	return y;
}

// Approximate 1st derivative with input and output expressed in dB.
// This slope is equal to the inverse of the compression "ratio".
// In other words, a compression ratio of 20 would be a slope of 1/20.
float CompressorNode::slopeAt( float x, float k )
{
	if( x < m_linearThreshold )
		return 1;

	float x2 = x * 1.001f;

	float xDb = linearToDecibels( x );
	float x2Db = linearToDecibels( x2 );

	float yDb = linearToDecibels( kneeCurve( x, k ) );
	float y2Db = linearToDecibels( kneeCurve( x2, k ) );

	float m = ( y2Db - yDb ) / ( x2Db - xDb );

	return m;
}

float CompressorNode::kAtSlope( float desiredSlope )
{
	float xDb = m_dbThreshold + m_dbKnee;
	float x = decibelsToLinear(xDb);

	// Approximate k given initial values.
	float minK = 0.1;
	float maxK = 10000;
	float k = 5;

	for( int i = 0; i < 15; ++i ) {
		// A high value for k will more quickly asymptotically approach a slope of 0.
		float slope = slopeAt( x, k );

		if( slope < desiredSlope ) {
			// k is too high.
			maxK = k;
		}
		else {
			// k is too low.
			minK = k;
		}

		// Re-calculate based on geometric mean.
		k = sqrtf( minK * maxK );
	}

	return k;
}

float CompressorNode::updateStaticCurveParameters( float dbThreshold, float dbKnee, float ratio )
{
	if( dbThreshold != m_dbThreshold || dbKnee != m_dbKnee || ratio != m_ratio ) {
		// Threshold and knee.
		m_dbThreshold = dbThreshold;
		m_linearThreshold = decibelsToLinear(dbThreshold);
		m_dbKnee = dbKnee;

		// Compute knee parameters.
		m_ratio = ratio;
		m_slope = 1 / m_ratio;

		float k = kAtSlope(1 / m_ratio);

		m_kneeThresholdDb = dbThreshold + dbKnee;
		m_kneeThreshold = decibelsToLinear(m_kneeThresholdDb);

		m_ykneeThresholdDb = linearToDecibels(kneeCurve(m_kneeThreshold, k));

		m_K = k;
	}

	return m_K;
}

void CompressorNode::process( ci::audio::Buffer *buffer )
{
	unsigned numberOfChannels = buffer->getNumChannels();
	unsigned framesToProcess = buffer->getNumFrames();

	mParamKnee.eval();
	mParamRatio.eval();
	mParamAttackTime.eval();
	mParamReleaseTime.eval();
	mParamThreshold.eval();

	float dbKnee = mParamKnee.getValue();
	float ratio = mParamRatio.getValue();
	float attackTime = mParamAttackTime.getValue();
	float releaseTime = mParamReleaseTime.getValue();
	float dbThreshold = mParamThreshold.getValue();

	float preDelayTime = 0.006f;
	float dbPostGain = 0; // db
	float effectBlend = 1; // equal power crossfade

	float releaseZone1 = 0.09f;
	float releaseZone2 = 0.16f;
	float releaseZone3 = 0.42f;
	float releaseZone4 = 0.98f;

	float sampleRate = getSampleRate();

	float dryMix = 1 - effectBlend;
	float wetMix = effectBlend;

	float k = updateStaticCurveParameters(dbThreshold, dbKnee, ratio);

	// Makeup gain.
	float fullRangeGain = saturate(1, k);
	float fullRangeMakeupGain = 1 / fullRangeGain;

	// Empirical/perceptual tuning.
	fullRangeMakeupGain = powf( fullRangeMakeupGain, 0.6f );

	float masterLinearGain = decibelsToLinear(dbPostGain) * fullRangeMakeupGain;

	// Attack parameters.
	attackTime = std::max( 0.001f, attackTime );
	float attackFrames = attackTime * sampleRate;

	// Release parameters.
	float releaseFrames = sampleRate * releaseTime;

	// Detector release time.
	float satReleaseTime = 0.0025f;
	float satReleaseFrames = satReleaseTime * sampleRate;

	// Create a smooth function which passes through four points.

	// Polynomial of the form
	// y = a + b*x + c*x^2 + d*x^3 + e*x^4;

	float y1 = releaseFrames * releaseZone1;
	float y2 = releaseFrames * releaseZone2;
	float y3 = releaseFrames * releaseZone3;
	float y4 = releaseFrames * releaseZone4;

	// All of these coefficients were derived for 4th order polynomial curve fitting where the y values
	// match the evenly spaced x values as follows: (y1 : x == 0, y2 : x == 1, y3 : x == 2, y4 : x == 3)
	float kA = 0.9999999999999998f * y1 + 1.8432219684323923e-16f * y2 - 1.9373394351676423e-16f * y3 + 8.824516011816245e-18f * y4;
	float kB = -1.5788320352845888f * y1 + 2.3305837032074286f * y2 - 0.9141194204840429f * y3 + 0.1623677525612032f * y4;
	float kC = 0.5334142869106424f * y1 - 1.272736789213631f * y2 + 0.9258856042207512f * y3 - 0.18656310191776226f * y4;
	float kD = 0.08783463138207234f * y1 - 0.1694162967925622f * y2 + 0.08588057951595272f * y3 - 0.00429891410546283f * y4;
	float kE = -0.042416883008123074f * y1 + 0.1115693827987602f * y2 - 0.09764676325265872f * y3 + 0.028494263462021576f * y4;

	setPreDelayTime( preDelayTime );

	const int nDivisionFrames = 32;

	const int nDivisions = framesToProcess / nDivisionFrames;

	unsigned frameIndex = 0;
	for (int i = 0; i < nDivisions; ++i) {
		// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
		// Calculate desired gain
		// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

		// Fix gremlins.
		if( std::isnan( m_detectorAverage ) )
			m_detectorAverage = 1;
		if( std::isinf( m_detectorAverage ) )
			m_detectorAverage = 1;

		float desiredGain = m_detectorAverage;

		// Pre-warp so we get desiredGain after sin() warp below.
		float piOverTwoFloat = M_PI / 2.0;
		float scaledDesiredGain = asinf( desiredGain ) / piOverTwoFloat;

		// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
		// Deal with envelopes
		// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

		// envelopeRate is the rate we slew from current compressor level to the desired level.
		// The exact rate depends on if we're attacking or releasing and by how much.
		float envelopeRate;

		bool isReleasing = scaledDesiredGain > m_compressorGain;

		// compressionDiffDb is the difference between current compression level and the desired level.
		float compressionDiffDb = linearToDecibels(m_compressorGain / scaledDesiredGain);

		if( isReleasing ) {
			// Release mode - compressionDiffDb should be negative dB
			m_maxAttackCompressionDiffDb = -1;

			// Fix gremlins.
			if( std::isnan( compressionDiffDb ) )
				compressionDiffDb = -1;
			if( std::isinf( compressionDiffDb ) )
				compressionDiffDb = -1;

			// Adaptive release - higher compression (lower compressionDiffDb)  releases faster.

			// Contain within range: -12 -> 0 then scale to go from 0 -> 3
			float x = compressionDiffDb;
			x = std::max( -12.0f, x );
			x = std::min( 0.0f, x );
			x = 0.25f * ( x + 12 );

			// Compute adaptive release curve using 4th order polynomial.
			// Normal values for the polynomial coefficients would create a monotonically increasing function.
			float x2 = x * x;
			float x3 = x2 * x;
			float x4 = x2 * x2;
			float releaseFrames = kA + kB * x + kC * x2 + kD * x3 + kE * x4;

			const float spacingDb = 5;
			float dbPerFrame = spacingDb / releaseFrames;

			envelopeRate = decibelsToLinear(dbPerFrame);
		} else {
			// Attack mode - compressionDiffDb should be positive dB

			// Fix gremlins.
			if (std::isnan(compressionDiffDb))
				compressionDiffDb = 1;
			if (std::isinf(compressionDiffDb))
				compressionDiffDb = 1;

			// As long as we're still in attack mode, use a rate based off
			// the largest compressionDiffDb we've encountered so far.
			if (m_maxAttackCompressionDiffDb == -1 || m_maxAttackCompressionDiffDb < compressionDiffDb)
				m_maxAttackCompressionDiffDb = compressionDiffDb;

			float effAttenDiffDb = std::max( 0.5f, m_maxAttackCompressionDiffDb );

			float x = 0.25f / effAttenDiffDb;
			envelopeRate = 1 - powf( x, 1 / attackFrames );
		}

		// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
		// Inner loop - calculate shaped power average - apply compression.
		// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

		{
			int preDelayReadIndex = m_preDelayReadIndex;
			int preDelayWriteIndex = m_preDelayWriteIndex;
			float detectorAverage = m_detectorAverage;
			float compressorGain = m_compressorGain;

			int loopFrames = nDivisionFrames;
			while( loopFrames-- ) {
				float compressorInput = 0;

				// Predelay signal, computing compression amount from un-delayed version.
				for (unsigned i = 0; i < numberOfChannels; ++i) {
					float *delayBuffer = mPreDelayBuffer.getChannel( i );
					float undelayedSource = buffer->getChannel( i )[frameIndex];

					CI_ASSERT( preDelayWriteIndex < mPreDelayBuffer.getNumFrames() );

					delayBuffer[preDelayWriteIndex] = undelayedSource;

					float absUndelayedSource = undelayedSource > 0 ? undelayedSource : -undelayedSource;
					if (compressorInput < absUndelayedSource)
						compressorInput = absUndelayedSource;
				}

				// Calculate shaped power on undelayed input.

				float scaledInput = compressorInput;
				float absInput = scaledInput > 0 ? scaledInput : -scaledInput;

				// Put through shaping curve.
				// This is linear up to the threshold, then enters a "knee" portion followed by the "ratio" portion.
				// The transition from the threshold to the knee is smooth (1st derivative matched).
				// The transition from the knee to the ratio portion is smooth (1st derivative matched).
				float shapedInput = saturate( absInput, k );

				float attenuation = absInput <= 0.0001f ? 1 : shapedInput / absInput;

				float attenuationDb = -linearToDecibels(attenuation);
				attenuationDb = std::max( 2.0f, attenuationDb );

				float dbPerFrame = attenuationDb / satReleaseFrames;

				float satReleaseRate = decibelsToLinear(dbPerFrame) - 1;

				bool isRelease = (attenuation > detectorAverage);
				float rate = isRelease ? satReleaseRate : 1;

				detectorAverage += ( attenuation - detectorAverage ) * rate;
				detectorAverage = std::min( 1.0f, detectorAverage );

				// Fix gremlins.
				if (std::isnan(detectorAverage))
					detectorAverage = 1;
				if (std::isinf(detectorAverage))
					detectorAverage = 1;

				// Exponential approach to desired gain.
				if (envelopeRate < 1) {
					// Attack - reduce gain to desired.
					compressorGain += (scaledDesiredGain - compressorGain) * envelopeRate;
				} else {
					// Release - exponentially increase gain to 1.0
					compressorGain *= envelopeRate;
					compressorGain = std::min( 1.0f, compressorGain );
				}

				// Warp pre-compression gain to smooth out sharp exponential transition points.
				float postWarpCompressorGain = sinf(piOverTwoFloat * compressorGain);

				// Calculate total gain using master gain and effect blend.
				float totalGain = dryMix + wetMix * masterLinearGain * postWarpCompressorGain;

				// Calculate metering.
				float dbRealGain = 20 * log10(postWarpCompressorGain);
				if (dbRealGain < m_meteringGain)
					m_meteringGain = dbRealGain;
				else
					m_meteringGain += (dbRealGain - m_meteringGain) * m_meteringReleaseK;

				// Apply final gain.
				for (unsigned i = 0; i < numberOfChannels; ++i) {
					float* delayBuffer = mPreDelayBuffer.getChannel( i );
					buffer->getChannel( i )[frameIndex] = delayBuffer[preDelayReadIndex] * totalGain;
				}

				frameIndex++;
				preDelayReadIndex = (preDelayReadIndex + 1) & MaxPreDelayFramesMask;
				preDelayWriteIndex = (preDelayWriteIndex + 1) & MaxPreDelayFramesMask;
			}

			// Locals back to member variables.
			m_preDelayReadIndex = preDelayReadIndex;
			m_preDelayWriteIndex = preDelayWriteIndex;
			m_detectorAverage = detectorAverage;
			m_compressorGain = compressorGain;
		}
	}
}

} } // namespace mason::audio
