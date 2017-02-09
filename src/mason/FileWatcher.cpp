/*
 Copyright (c) 2014-15, Richard Eakin - All rights reserved.

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

#include "mason/FileWatcher.h"

#include "cinder/app/App.h"
#include "cinder/Log.h"

using namespace ci;
using namespace std;

namespace mason {

//! Base class for Watch types, which are returned from FileWatcher::load() and watch()
class Watch : public std::enable_shared_from_this<Watch>, private ci::Noncopyable {
public:

	virtual ~Watch() = default;

	//! Checks if the asset file is up-to-date, emitting a signal callback if not. Also may discard the Watch if there are no more connected slots.
	virtual void checkCurrent() = 0;
	//! Remove any watches for \a filePath. If it is the last file associated with this Watch, discard
	virtual void unwatch( const fs::path &filePath ) = 0;
	//! Emit the signal callback. 
	virtual void emitCallback() = 0;

	//! Marks the Watch as discarded, will be destroyed the next update loop
	void markDiscarded()			{ mDiscarded = true; }
	//! Returns whether the Watch is discarded and should be destroyed.
	bool isDiscarded() const		{ return mDiscarded; }

private:
	bool mDiscarded = false;
};

//! Handles a single live asset
class WatchSingle : public Watch {
  public:
	WatchSingle( const ci::fs::path &filePath );

	signals::Connection	connect( const function<void ( const ci::fs::path& )> &callback )	{ return mSignalChanged.connect( callback ); }

	void checkCurrent() override;
	void unwatch( const fs::path &filePath ) override;
	void emitCallback() override;

  protected:
	ci::signals::Signal<void ( const ci::fs::path& )>	mSignalChanged;
	ci::fs::path										mFilePath;
	ci::fs::file_time_type								mTimeLastWrite;
};

//! Handles multiple live assets. Takes a vector of fs::paths as argument, result function gets an array of resolved filepaths.
class WatchMany : public Watch {
  public:
	WatchMany( const std::vector<ci::fs::path> &filePaths );

	signals::Connection	connect( const function<void ( const vector<fs::path>& )> &callback )	{ return mSignalChanged.connect( callback ); }

	void checkCurrent() override;
	void unwatch( const fs::path &filePath ) override;
	void emitCallback() override;

	size_t	getNumFiles() const	{ return mFilePaths.size(); }

  protected:
	ci::signals::Signal<void ( const std::vector<ci::fs::path>& )>	mSignalChanged;

	std::vector<ci::fs::path>			mFilePaths;
	std::vector<ci::fs::file_time_type>	mTimeStamps;
};

namespace {

fs::path findFullFilePath( const fs::path &filePath )
{
	if( filePath.empty() )
		throw FileWatcherException( "empty path" );

	if( filePath.is_absolute() && fs::exists( filePath ) )
		return filePath;

	auto resolvedAssetPath = app::getAssetPath( filePath );
	if( ! fs::exists( resolvedAssetPath ) )
		throw FileWatcherException( "could not resolve file path: " + filePath.string() );

	return resolvedAssetPath;
}
	
} // anonymous namespace

// ----------------------------------------------------------------------------------------------------
// WatchSingle
// ----------------------------------------------------------------------------------------------------

WatchSingle::WatchSingle( const fs::path &filePath )
{
	mFilePath = findFullFilePath( filePath );
	mTimeLastWrite = fs::last_write_time( mFilePath );
}

void WatchSingle::checkCurrent()
{
	if( ! fs::exists( mFilePath ) )
		return;

	// Discard when there are no more connected slots
	if( mSignalChanged.getNumSlots() == 0 ) {
		markDiscarded();
		return;
	}

	auto timeLastWrite = fs::last_write_time( mFilePath );
	if( mTimeLastWrite < timeLastWrite ) {
		mTimeLastWrite = timeLastWrite;
		mSignalChanged.emit( mFilePath );
	}
}

void WatchSingle::unwatch( const fs::path &filePath ) 
{
	if( mFilePath == filePath )
		markDiscarded();
}

void WatchSingle::emitCallback() 
{ 
	mSignalChanged.emit( mFilePath ); 
} 

// ----------------------------------------------------------------------------------------------------
// WatchMany
// ----------------------------------------------------------------------------------------------------

WatchMany::WatchMany( const vector<fs::path> &filePaths )
{
	mFilePaths.reserve( filePaths.size() );
	mTimeStamps.reserve( filePaths.size() );
	for( const auto &fp : filePaths ) {
		auto fullPath = findFullFilePath( fp );
		mFilePaths.push_back( fullPath );
		mTimeStamps.push_back( fs::last_write_time( fullPath ) );
	}
}

void WatchMany::checkCurrent()
{
	// Discard when there are no more connected slots
	if( mSignalChanged.getNumSlots() == 0 ) {
		markDiscarded();
		return;
	}

	const size_t numFiles = mFilePaths.size();
	for( size_t i = 0; i < numFiles; i++ ) {
		const fs::path &fp = mFilePaths[i];
		if( fs::exists( fp ) ) {
			auto timeLastWrite = fs::last_write_time( fp );
			auto &currentTimeLastWrite = mTimeStamps[i];
			if( currentTimeLastWrite < timeLastWrite ) {
				currentTimeLastWrite = timeLastWrite;
				mSignalChanged.emit( mFilePaths );
			}
		}
	}
}

void WatchMany::unwatch( const fs::path &filePath ) 
{
	mFilePaths.erase( remove( mFilePaths.begin(), mFilePaths.end(), filePath ), mFilePaths.end() );
	
	if( mFilePaths.empty() )
		markDiscarded();
}

void WatchMany::emitCallback() 
{ 
	mSignalChanged.emit( mFilePaths ); 
} 

// ----------------------------------------------------------------------------------------------------
// FileWatcher
// ----------------------------------------------------------------------------------------------------

namespace {

bool	sWatchingEnabled = true;

} // anonymous namespace

// static
FileWatcher* FileWatcher::instance()
{
	static FileWatcher sInstance;
	return &sInstance;
}

FileWatcher::FileWatcher()
{
	if( sWatchingEnabled )
		connectUpdate();
}

void FileWatcher::connectUpdate()
{
	if( app::App::get() && ! mUpdateConn.isConnected() )
		mUpdateConn = app::App::get()->getSignalUpdate().connect( bind( &FileWatcher::update, this ) );
}

// static
void FileWatcher::setWatchingEnabled( bool enable )
{
	sWatchingEnabled = enable;
	if( enable )
		instance()->connectUpdate();
	else
		instance()->mUpdateConn.disconnect();
}

// static
bool FileWatcher::isWatchingEnabled()
{
	return sWatchingEnabled;
}

// static
signals::Connection FileWatcher::load( const fs::path &filePath, const function<void( const fs::path& )> &callback )
{
	auto watch = make_shared<WatchSingle>( filePath );

	auto fw = instance();
	lock_guard<recursive_mutex> lock( fw->mMutex );

	fw->mWatchList.push_back( watch );
	auto conn = watch->connect( callback );
	watch->emitCallback();
	return conn;
}

// static
signals::Connection FileWatcher::load( const vector<fs::path> &filePaths, const function<void ( const vector<fs::path>& )> &callback )
{
	auto watch = make_shared<WatchMany>( filePaths );

	auto fw = instance();
	lock_guard<recursive_mutex> lock( fw->mMutex );

	fw->mWatchList.push_back( watch );
	auto conn = watch->connect( callback );
	watch->emitCallback();
	return conn;
}

// static
signals::Connection FileWatcher::watch( const fs::path &filePath, const function<void( const fs::path& )> &callback )
{
	auto watch = make_shared<WatchSingle>( filePath );

	auto fw = instance();
	lock_guard<recursive_mutex> lock( fw->mMutex );

	fw->mWatchList.push_back( watch );
	return watch->connect( callback );
}

// static
signals::Connection FileWatcher::watch( const vector<fs::path> &filePaths, const function<void ( const vector<fs::path>& )> &callback )
{
	auto watch = make_shared<WatchMany>( filePaths );

	auto fw = instance();
	lock_guard<recursive_mutex> lock( fw->mMutex );

	fw->mWatchList.push_back( watch );
	return watch->connect( callback );
}

// static
void FileWatcher::unwatch( const fs::path &filePath )
{
	for( auto &watch : instance()->mWatchList ) {
		watch->unwatch( filePath );
	}
}

// static
void FileWatcher::unwatch( const vector<fs::path> &filePaths )
{
	for( auto &watch : instance()->mWatchList ) {
		for( const auto &filePath : filePaths )
			watch->unwatch( filePath );
	}
}

void FileWatcher::update()
{
	// try-lock, if we fail to acquire the mutex then we skip this update
	unique_lock<recursive_mutex> lock( mMutex, std::try_to_lock );
	if( ! lock.owns_lock() )
		return;

	try {
		auto it = mWatchList.begin();
		while( it != mWatchList.end() ) {
			auto watch = *it;
			CI_ASSERT( watch );

			if( watch->isDiscarded() ) {
				it = mWatchList.erase( it );
				continue;
			}

			watch->checkCurrent();
			++it;
		}
	}
	catch( fs::filesystem_error & ) {
		// some file probably got locked by the system. Do nothing this update frame, we'll check again next
	}
}

const size_t FileWatcher::getNumWatchedFiles() const
{
	size_t result = 0;
	for( const auto &w : mWatchList ) {
		auto many = dynamic_pointer_cast<WatchMany>( w );
		if( many )
			result += many->getNumFiles();
		else
			result += 1;
	}

	return result;
}

} // namespace mason
