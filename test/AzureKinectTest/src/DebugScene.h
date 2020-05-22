#pragma once

#include "ck4a/CaptureManager.h"

#include "cinder/AxisAlignedBox.h"
#include "cinder/Color.h"
#include "cinder/gl/Batch.h"

#include "mason/Info.h"
#include "mason/FlyCam.h"

class DebugScene {
public:
	void load( const ma::Info &info, bool full );
	void save( ma::Info &info ) const;

	void draw( const ck4a::CaptureManager *capture );
	void updateUI();

private:
	void initScene();
	void initCam();
	void loadSceneObjects();

	void drawCaptureDevices( const ck4a::CaptureManager *capture );
	void drawDeviceBodies( const ck4a::CaptureManager *capture );
	void drawBody( const ck4a::Body &body, const ci::Color &boneColor );

	ci::signals::ConnectionList		mConnections;
	ci::AxisAlignedBox				mWorldBounds;
	ma::FlyCam						mFlyCam;
	ci::CameraPersp					mCam, mInitialCam;
	bool							mCamInitialized = false;

	struct SceneBox {
		bool				mHitTest = false;
		ci::AxisAlignedBox	mBox = ci::AxisAlignedBox( ci::vec3( -10 ), ci::vec3( 10 ) );
		ci::Color			mColor = ci::Color( 0.25f, 0.12f, 0.05f );
	};

	std::vector<SceneBox>	mBoxes;

	ci::gl::BatchRef	mBatchBackground, mBatchRoomWires, mBatchFloor, mBatchSphere, mBatchCube, mBatchArrow;

	bool	mDrawEnabled = true;
	bool	mUIEnabled = true;
	bool	mDrawDebugCaptureDevices = true;
	bool	mDrawRoom = true;
	bool	mDrawDeviceBodies = false; //! Draw individual bodies from each camera
	bool	mDrawMergedBodies = true; // TODO: rename to mDrawBodies

	bool mDrawHitTestBox = false;
	bool mDrawJointOrientations = true;
	bool mDrawHeadLazerBeam = false;
	bool mDrawExtendedJoints = false; // eyes, ears, toes, thumbs

	ci::vec3 mMarkerPos;
};