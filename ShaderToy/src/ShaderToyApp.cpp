/*
 Copyright (c) 2014, Paul Houx - All rights reserved.
 This code is intended for use with the Cinder C++ library: http://libcinder.org

 Redistribution and use in source and binary forms, with or without modification, are permitted provided that
 the following conditions are met:

    * Redistributions of source code must retain the above copyright notice, this list of conditions and
	the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright notice, this list of conditions and
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

#pragma warning(push)
#pragma warning(disable: 4996) // _CRT_SECURE_NO_WARNINGS

#include "cinder/app/AppNative.h"
#include "cinder/gl/gl.h"
#include "cinder/gl/Fbo.h"
#include "cinder/gl/GlslProg.h"
#include "cinder/gl/Texture.h"
#include "cinder/ConcurrentCircularBuffer.h"
#include "cinder/ImageIo.h"
#include "cinder/Utilities.h"
#include <ctime>

#if !defined( CINDER_MSW )
	#error Due to the use of shared GL contexts, this sample is for Windows only. See: http://tinyurl.com/m9557x2
#endif

using namespace ci;
using namespace ci::app;
using namespace std;

class ShaderToyApp : public AppNative {
public:
	void prepareSettings( Settings* settings );
	void setup();
	void shutdown();

	void update();
	void draw();

	void mouseDown( MouseEvent event );
	void mouseDrag( MouseEvent event );

	void keyDown( KeyEvent event );

	void resize();
	void fileDrop( FileDropEvent event );

	void random();
private:
	void setupLoader();
	void shutdownLoader();

#if defined( CINDER_MSW )
	void loader( HDC hdc, HGLRC renderContext );
#else
	#error Not implemented for this platform.
#endif
private:
	//! Time in seconds at which the transition to the next shader starts.
	double          mTransitionTime;
	//! Duration in seconds of the transition to the next shader.
	double          mTransitionDuration;
	//! Shader that will perform the transition to the next shader.
	gl::GlslProgRef mShaderTransition;
	//! Currently active shader.
	gl::GlslProgRef mShaderCurrent;
	//! Shader that has just been loaded and we are transitioning to.
	gl::GlslProgRef mShaderNext;
	//! Buffer containing the rendered output of the currently active shader.
	gl::Fbo         mBufferCurrent;
	//! Buffer containing the rendered output of the shader we are transitioning to.
	gl::Fbo         mBufferNext;
	//! Texture slots for our shader, based on ShaderToy.
	gl::TextureRef  mChannel0;
	gl::TextureRef  mChannel1;
	gl::TextureRef  mChannel2;
	gl::TextureRef  mChannel3;
	//! Our mouse position: xy = current position while mouse down, zw = last click position.
	Vec4f           mMouse;
	//! The main thread will push a file path to this buffer, to be picked up by the loading thread.
	ConcurrentCircularBuffer<fs::path>*        mRequests;
	//! The loading thread will push a shader to this buffer, to be picked up by the main thread.
	ConcurrentCircularBuffer<gl::GlslProgRef>* mResponses;
	//! Our loading thread, sharing a OpenGL context with the main thread.
	std::shared_ptr<std::thread>               mThread;
	//! Signals if the loading thread should abort.
	bool                                       mThreadAbort;
};

void ShaderToyApp::prepareSettings(Settings* settings)
{
	// Do not allow resizing our window. Feel free to remove this limitation.
	settings->setResizable(false);
}

void ShaderToyApp::setup()
{
	// Create our buffers.
	mRequests = new ConcurrentCircularBuffer<fs::path>(10);
	mResponses = new ConcurrentCircularBuffer<gl::GlslProgRef>(10);

	// Start the loading thread.
	setupLoader();

	// Tell our loading thread to load the first shader
	random();

	// Load our textures and transition shader in the main thread.
	try {
		gl::Texture::Format fmt;
		fmt.setWrap( GL_REPEAT, GL_REPEAT );

		mChannel0 = gl::Texture::create( loadImage( loadAsset("presets/tex16.png") ), fmt );
		mChannel1 = gl::Texture::create( loadImage( loadAsset("presets/tex06.jpg") ), fmt );
		mChannel2 = gl::Texture::create( loadImage( loadAsset("presets/tex09.jpg") ), fmt );
		mChannel3 = gl::Texture::create( loadImage( loadAsset("presets/tex02.jpg") ), fmt );

		mShaderTransition = gl::GlslProg::create( loadAsset("common/shadertoy.vert"), loadAsset("common/shadertoy.frag") );
	}
	catch( const std::exception& e ) {
		// Quit if anything went wrong.
		console() << e.what() << endl;
		quit();
	}
}

void ShaderToyApp::shutdown()
{
	// Properly shut down the loading thread.
	shutdownLoader();
}

void ShaderToyApp::update()
{
	// If we are ready for the next shader, take it from the buffer.
	if(!mShaderNext && mResponses->isNotEmpty()) {
		mResponses->popBack(&mShaderNext);

		mTransitionTime = getElapsedSeconds() + 1.0;
		mTransitionDuration = 2.5;
	}
}

void ShaderToyApp::draw()
{
	// Calculate shader parameters.
	Vec3f iResolution( Vec2f( getWindowSize() ), 1.f );
	float iGlobalTime = (float) getElapsedSeconds();
	float iChannelTime0 = (float) getElapsedSeconds();
	float iChannelTime1 = (float) getElapsedSeconds();
	float iChannelTime2 = (float) getElapsedSeconds();
	float iChannelTime3 = (float) getElapsedSeconds();
	Vec3f iChannelResolution0 = mChannel0 ? Vec3f( mChannel0->getSize(), 1.f ) : Vec3f::one();
	Vec3f iChannelResolution1 = mChannel1 ? Vec3f( mChannel1->getSize(), 1.f ) : Vec3f::one();
	Vec3f iChannelResolution2 = mChannel2 ? Vec3f( mChannel2->getSize(), 1.f ) : Vec3f::one();
	Vec3f iChannelResolution3 = mChannel3 ? Vec3f( mChannel3->getSize(), 1.f ) : Vec3f::one();

	time_t now = time(0);
	tm*    t = gmtime(&now);
	Vec4f iDate( float(t->tm_year + 1900),
				 float(t->tm_mon + 1),
				 float(t->tm_mday),
				 float(t->tm_hour * 3600 + t->tm_min * 60 + t->tm_sec) );

	// Render the current shader.
	if(mShaderCurrent && mBufferCurrent) {
		mBufferCurrent.bindFramebuffer();

		mShaderCurrent->bind();
		mShaderCurrent->uniform("iResolution", iResolution);
		mShaderCurrent->uniform("iGlobalTime", iGlobalTime);
		mShaderCurrent->uniform("iChannelTime[0]", iChannelTime0);
		mShaderCurrent->uniform("iChannelTime[1]", iChannelTime1);
		mShaderCurrent->uniform("iChannelTime[2]", iChannelTime2);
		mShaderCurrent->uniform("iChannelTime[3]", iChannelTime3);
		mShaderCurrent->uniform("iChannelResolution[0]", iChannelResolution0);
		mShaderCurrent->uniform("iChannelResolution[1]", iChannelResolution1);
		mShaderCurrent->uniform("iChannelResolution[2]", iChannelResolution2);
		mShaderCurrent->uniform("iChannelResolution[3]", iChannelResolution3);
		mShaderCurrent->uniform("iMouse", mMouse);
		mShaderCurrent->uniform("iChannel0", 0);
		mShaderCurrent->uniform("iChannel1", 1);
		mShaderCurrent->uniform("iChannel2", 2);
		mShaderCurrent->uniform("iChannel3", 3);
		mShaderCurrent->uniform("iDate", iDate);

		//
		if(mChannel0) mChannel0->bind(0);
		if(mChannel1) mChannel1->bind(1);
		if(mChannel2) mChannel2->bind(2);
		if(mChannel3) mChannel3->bind(3);

		//
		gl::clear();
		gl::draw( mChannel0, mBufferCurrent.getBounds() );

		//
		mShaderCurrent->unbind();
		mBufferCurrent.unbindFramebuffer();
	}
	
	// Render the next shader.
	if(mShaderNext && mBufferNext) {
		mBufferNext.bindFramebuffer();

		mShaderNext->bind();
		mShaderNext->uniform("iResolution", iResolution);
		mShaderNext->uniform("iGlobalTime", iGlobalTime);
		mShaderNext->uniform("iChannelTime[0]", iChannelTime0);
		mShaderNext->uniform("iChannelTime[1]", iChannelTime1);
		mShaderNext->uniform("iChannelTime[2]", iChannelTime2);
		mShaderNext->uniform("iChannelTime[3]", iChannelTime3);
		mShaderNext->uniform("iChannelResolution[0]", iChannelResolution0);
		mShaderNext->uniform("iChannelResolution[1]", iChannelResolution1);
		mShaderNext->uniform("iChannelResolution[2]", iChannelResolution2);
		mShaderNext->uniform("iChannelResolution[3]", iChannelResolution3);
		mShaderNext->uniform("iMouse", mMouse);
		mShaderNext->uniform("iChannel0", 0);
		mShaderNext->uniform("iChannel1", 1);
		mShaderNext->uniform("iChannel2", 2);
		mShaderNext->uniform("iChannel3", 3);
		mShaderNext->uniform("iDate", iDate);

		//
		if(mChannel0) mChannel0->bind(0);
		if(mChannel1) mChannel1->bind(1);
		if(mChannel2) mChannel2->bind(2);
		if(mChannel3) mChannel3->bind(3);

		//
		gl::clear();
		gl::draw( mChannel0, mBufferNext.getBounds() );

		//
		mShaderNext->unbind();
		mBufferNext.unbindFramebuffer();
	}

	// Perform a cross-fade between the two shaders.
	double time = getElapsedSeconds() - mTransitionTime;
	double fade = math<double>::clamp( time / mTransitionDuration, 0.0, 1.0 );

	if(fade <= 0.0) {
		// Transition has not yet started. Keep drawing current buffer.
		gl::draw( mBufferCurrent.getTexture(), getWindowBounds() );
	}
	else if(fade < 1.0) {
		// Transition is in progress.
		// Use a transition shader to avoid having to draw one buffer on top of another.
		mBufferNext.getTexture().bind(1);

		mShaderTransition->bind();
		mShaderTransition->uniform("iSrc", 0);
		mShaderTransition->uniform("iDst", 1);
		mShaderTransition->uniform("iFade", (float)fade);

		gl::draw( mBufferCurrent.getTexture(), getWindowBounds() );

		mShaderTransition->unbind();
	}
	else if(mShaderNext) {
		// Transition is done. Swap shaders.
		gl::draw( mBufferNext.getTexture(), getWindowBounds() );

		mShaderCurrent = mShaderNext;
		mShaderNext.reset();
	}
	else {
		// No transition in progress.
		gl::draw( mBufferCurrent.getTexture(), getWindowBounds() );
	}
}

void ShaderToyApp::mouseDown( MouseEvent event )
{
	mMouse.x = (float) event.getPos().x;
	mMouse.y = (float) event.getPos().y;
	mMouse.z = (float) event.getPos().x;
	mMouse.w = (float) event.getPos().y;
}

void ShaderToyApp::mouseDrag( MouseEvent event )
{
	mMouse.x = (float) event.getPos().x;
	mMouse.y = (float) event.getPos().y;
}

void ShaderToyApp::keyDown( KeyEvent event )
{
	switch( event.getCode() )
	{
	case KeyEvent::KEY_ESCAPE:
		quit();
		break;
	case KeyEvent::KEY_SPACE:
		random();
		break;
	}
}

void ShaderToyApp::resize()
{
	// Create/resize frame buffers (no multisampling)
	mBufferCurrent = gl::Fbo( getWindowWidth(), getWindowHeight() );
	mBufferNext = gl::Fbo( getWindowWidth(), getWindowHeight() );

	mBufferCurrent.getTexture().setFlipped(true);
	mBufferNext.getTexture().setFlipped(true);
}

void ShaderToyApp::fileDrop( FileDropEvent event )
{
	// Send all file requests to the loading thread.
	size_t count = event.getNumFiles();
	for(size_t i=0;i<count && mRequests->isNotFull();++i)
		mRequests->pushFront( event.getFile(i) );
}

void ShaderToyApp::random()
{
	const fs::path assets = getAssetPath("");

	// Find all *.frag files.
	std::vector<fs::path> shaders;
	for (fs::directory_iterator it(assets), end; it != end; ++it)
	{
		if (fs::is_regular_file(it->path()))
			if(it->path().extension() == ".frag")
				shaders.push_back(it->path());
	}

	if(shaders.empty())
		return;

	// Load random *.frag file.
	const size_t idx = getElapsedFrames() % shaders.size();
	if(mRequests->isNotFull())
		mRequests->pushFront( shaders.at(idx) );
}

void ShaderToyApp::setupLoader()
{
#if defined( CINDER_MSW )
	// Check if the device context is available.
	HDC hdc = ::GetDC( (HWND) app::getWindow()->getNative() );
	if(!hdc) return;

	// Create a second OpenGL context and share its lists with the main context.
	HGLRC renderContext = ::wglCreateContext(hdc);
	if(!renderContext) return;

	HGLRC mainContext = ::wglGetCurrentContext();
	if(!mainContext) return;

	if( SUCCEEDED(::wglShareLists( mainContext, renderContext )) )
	{
		// If succeeded, start the loading thread.
		mThreadAbort = false;
		mThread = std::make_shared<std::thread>(&ShaderToyApp::loader, this, hdc, renderContext);
	}
#else
	#error Not implemented for this platform.
#endif
}

void ShaderToyApp::shutdownLoader()
{
	// Tell the loading thread to abort, then wait for it to stop.
	mThreadAbort = true;
	mThread->join();
}

#if defined( CINDER_MSW )
void ShaderToyApp::loader( HDC hdc, HGLRC renderContext )
{
	// This only works if we can make the render context current.
	if( !SUCCEEDED(::wglMakeCurrent( hdc, renderContext )) )
	return;

	// Loading loop.
	while(!mThreadAbort)
	{
		// Wait for a request.
		if(mRequests->isNotEmpty())
		{
			// Take the request from the buffer.
			fs::path path;
			mRequests->popBack(&path);

			// Try to load, parse and compile the shader.
			try {
				std::string vs = loadString( loadAsset("common/shadertoy.vert") );
				std::string fs = loadString( loadAsset("common/shadertoy.inc") ) + loadString( loadFile( path ) );

				gl::GlslProgRef shader = gl::GlslProg::create( vs.c_str(), fs.c_str() );

				// If the shader compiled successfully, pass it to the main thread.
				mResponses->pushFront(shader);
			}
			catch( const std::exception& e ) {
				// Uhoh, something went wrong.
				console() << e.what() << endl;
			}
		}
		else {
			// Allow the CPU to do other things for a while.
			std::chrono::milliseconds duration( 100 );
			std::this_thread::sleep_for( duration );
		}
	}

	// Delete render context when done.
	::wglDeleteContext( renderContext );
	renderContext = NULL;
}
#else
	#error Not implemented for this platform.
#endif

#pragma warning(pop) // _CRT_SECURE_NO_WARNINGS

// Disable multisampling for better performance.
CINDER_APP_NATIVE( ShaderToyApp, RendererGl(RendererGl::AA_NONE) )
