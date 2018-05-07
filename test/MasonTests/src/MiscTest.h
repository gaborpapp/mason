#pragma once

#include "ui/Suite.h"

#include "mason/Mason.h"
#include "cinder/FileWatcher.h"
#include "mason/Dictionary.h"
#include "ui/Control.h"

class MiscTest : public ui::SuiteView {
  public:
	MiscTest();

	void layout() override;
	void update() override;
	void draw( ui::Renderer *ren )	override;

	bool keyDown( ci::app::KeyEvent &event ) override;

  private:
	void testDict( const ma::Dictionary &dict );
	void testConvertBack( const ma::Dictionary &dict );
	void testPrintingDict();
	void testMergegDict();

	void addStressTestWatches();

	ci::signals::ScopedConnection		mConnDict;
};
