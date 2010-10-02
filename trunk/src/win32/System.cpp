// System.cpp : Defines the system behaviors for the emulator.
//
#include "stdafx.h"
#include "Sound.h"
#include "Input.h"
#include "IUpdate.h"
#include "WinResUtil.h"
#include "resource.h"
#include "VBA.h"
#include "../gba/GBA.h"
#include "../gba/GBAGlobals.h"
#include "../gba/GBASound.h"
#include "../gb/GB.h"
#include "../gb/gbGlobals.h"
//#include "../common/System.h"
#include "../common/Text.h"
#include "../common/Util.h"
#include "../common/nesvideos-piece.h"
#include "../version.h"
#include <cassert>

int  RGB_LOW_BITS_MASK = 0;

int  emulating       = 0;

int  systemSpeed     = 0;
bool systemSoundOn   = false;
u32  systemColorMap32[0x10000];
u16  systemColorMap16[0x10000];
u16  systemGbPalette[24];
int  systemRedShift = 0;
int  systemBlueShift         = 0;
int  systemGreenShift        = 0;
int  systemColorDepth        = 16;
int  systemDebug             = 0;
int  systemVerbose           = 0;
int  systemFrameSkip = 0;
int  systemSaveUpdateCounter = SYSTEM_SAVE_NOT_UPDATED;

#define BMP_BUFFER_MAX_WIDTH (256)
#define BMP_BUFFER_MAX_HEIGHT (224)
#define BMP_BUFFER_MAX_DEPTH (4)
static u8 bmpBuffer[BMP_BUFFER_MAX_WIDTH * BMP_BUFFER_MAX_HEIGHT * BMP_BUFFER_MAX_DEPTH];

// joypad

int systemGetDefaultJoypad()
{
	return theApp.joypadDefault;
}

bool systemReadJoypads()
{
	// this function is called at every frame, even if vba is fast-forwarded.
	// so we try to limit the input frequency here just in case.
	static u32 lastTime = systemGetClock();
	if ((u32)(systemGetClock() - lastTime) < 10)
		return true;  // must return true because it's related to movie timing
	lastTime = systemGetClock();

	if (theApp.input)
		return theApp.input->readDevices();
	return false;
}

u32 systemGetJoypad(int which, bool sensor)
{
	if (theApp.input /* || VBALuaUsingJoypad(which)*/)
		return theApp.input->readDevice(which, sensor, false);
	return 0;
}

void systemSetJoypad(int which, u32 buttons)
{
	if (which < 0 || which > 3)
		which = theApp.joypadDefault;

	// TODO
}


// screen

void systemRefreshScreen()
{
	theApp.m_pMainWnd->PostMessage(WM_PAINT, NULL, NULL);
}

extern bool vbaShuttingDown;
extern long linearSoundFrameCount;
long		linearFrameCount = 0;

void systemRenderFrame()
{
	if (vbaShuttingDown)
		return;

	++theApp.renderedFrames;

	// "in-game" text rendering
	if (textMethod == 0) // transparent text can only be painted once, so timed messages will not be updated
	{
		extern void DrawLuaGui();
		DrawLuaGui();   // huh?

		int copyX = 240, copyY = 160;
		if (theApp.cartridgeType == 1)
			if (gbBorderOn)
				copyX = 256, copyY = 224;
			else
				copyX = 160, copyY = 144;
		int pitch = copyX * (systemColorDepth / 8) + (systemColorDepth == 24 ? 0 : 4);  // FIXME: sure?

		DrawTextMessages((u8 *)pix, pitch, 0, copyY);
	}

	++linearFrameCount;
	if (!theApp.sound)
	{
		if (linearFrameCount > 10000)
			linearFrameCount -= 10000;
		linearSoundFrameCount = linearFrameCount;
	}

	// record avi
	int width  = 240;
	int height = 160;
	switch (theApp.cartridgeType)
	{
	case 0:
		width  = 240;
		height = 160;
		break;
	case 1:
		if (gbBorderOn)
		{
			width  = 256;
			height = 224;
		}
		else
		{
			width  = 160;
			height = 144;
		}
		break;
	}

	bool firstFrameLogged = false;
	--linearFrameCount;
	do
	{
		++linearFrameCount;

		if (theApp.aviRecording && (!theApp.altAviRecordMethod || (theApp.altAviRecordMethod && !firstFrameLogged)))
		{
			// usually aviRecorder is created when vba starts avi recording, though
			if (theApp.aviRecorder == NULL)
			{
				theApp.aviRecorder = new AVIWrite();

				theApp.aviRecorder->SetFPS(60);

				BITMAPINFOHEADER bi;
				memset(&bi, 0, sizeof(bi));
				bi.biSize	   = 0x28;
				bi.biPlanes	   = 1;
				bi.biBitCount  = 24;
				bi.biWidth	   = width;
				bi.biHeight	   = height;
				bi.biSizeImage = 3 * width * height;
				theApp.aviRecorder->SetVideoFormat(&bi);
				if (!theApp.aviRecorder->Open(theApp.aviRecordName))
				{
					delete theApp.aviRecorder;
					theApp.aviRecorder	= NULL;
					theApp.aviRecording = false;
				}
			}

			if (theApp.aviRecorder != NULL && !theApp.aviRecorder->IsPaused())
			{
				assert(
				    width <= BMP_BUFFER_MAX_WIDTH && height <= BMP_BUFFER_MAX_HEIGHT && systemColorDepth <=
				    BMP_BUFFER_MAX_DEPTH * 8);
				utilWriteBMP(bmpBuffer, width, height, systemColorDepth, pix);
				theApp.aviRecorder->AddFrame(bmpBuffer);
			}
		}

		if (theApp.nvVideoLog)
		{
			// convert from whatever bit depth to 16-bit, while stripping away extra pixels
			assert(width <= BMP_BUFFER_MAX_WIDTH && height <= BMP_BUFFER_MAX_HEIGHT && 16 <= BMP_BUFFER_MAX_DEPTH * 8);
			utilWriteBMP(bmpBuffer, width, -height, 16, pix);
			NESVideoLoggingVideo((u8 *)bmpBuffer, width, height, 0x1000000 * 60);
		}

		firstFrameLogged = true;
	}
	while (linearFrameCount < linearSoundFrameCount); // compensate for frames lost due to frame skip being nonzero, etc.

	// interframe blending
	if (theApp.ifbFunction)
	{
		if (systemColorDepth == 16)
			theApp.ifbFunction(pix + theApp.filterWidth * 2 + 4, theApp.filterWidth * 2 + 4,
			                   theApp.filterWidth, theApp.filterHeight);
		else
			theApp.ifbFunction(pix + theApp.filterWidth * 4 + 4, theApp.filterWidth * 4 + 4,
			                   theApp.filterWidth, theApp.filterHeight);
	}

	systemRedrawScreen();
}

void systemRedrawScreen()
{
	if (vbaShuttingDown)
		return;

	if (theApp.display)
		theApp.display->render();

	// update viewers etc.
	if (theApp.updateCount)
	{
		POSITION pos = theApp.updateList.GetHeadPosition();
		while (pos)
		{
			IUpdateListener *up = theApp.updateList.GetNext(pos);
			if (up)
				up->update();
		}
	}
}

void systemScreenCapture(int captureNumber)
{
	if (theApp.m_pMainWnd)
		((MainWnd *)theApp.m_pMainWnd)->screenCapture(captureNumber);

	//if (theApp.m_pMainWnd)
	//	theApp.m_pMainWnd->PostMessage(WM_COMMAND, (WPARAM)ID_FILE_QUICKSCREENCAPTURE, (LPARAM)NULL);
}

void systemMessage(int number, const char *defaultMsg, ...)
{
	CString buffer;
	va_list valist;
	CString msg = defaultMsg;
	if (number)
		msg = winResLoadString(number);

	va_start(valist, defaultMsg);
	buffer.FormatV(msg, valist);

	theApp.winCheckFullscreen();
	if (theApp.sound) theApp.sound->clearAudioBuffer();

	AfxGetApp()->m_pMainWnd->MessageBox(buffer, winResLoadString(IDS_ERROR), MB_OK|MB_ICONERROR);

	va_end(valist);
}

void systemScreenMessage(const char *msg, int slot, int duration, const char *colorList)
{
	if (slot < 0 || slot > SCREEN_MESSAGE_SLOTS)
		return;

	theApp.screenMessage[slot] = true;
	theApp.screenMessageTime[slot]		  = GetTickCount();
	theApp.screenMessageDuration[slot]	  = duration;
	theApp.screenMessageBuffer[slot]	  = msg;
	theApp.screenMessageColorBuffer[slot] = colorList ? colorList : "";

	if (theApp.screenMessageBuffer[slot].GetLength() > 40)
		theApp.screenMessageBuffer[slot] = theApp.screenMessageBuffer[slot].Left(40);

	// update the display when a main slot message appears while the game is paused
	if (slot == 0 && (theApp.paused || (theApp.frameSearching)))
		systemRefreshScreen();
}

void systemShowSpeed(int speed)
{
	systemSpeed = speed;
	theApp.showRenderedFrames = theApp.renderedFrames;
	theApp.renderedFrames	  = 0;
	if (theApp.videoOption <= VIDEO_4X && theApp.showSpeed)
	{
		CString buffer;
		if (theApp.showSpeed == 1)
			buffer.Format(VBA_NAME_AND_VERSION "-%3d%%", systemSpeed);
		else
			buffer.Format(VBA_NAME_AND_VERSION "-%3d%% (%d fps | %d skipped)",
			              systemSpeed,
			              theApp.showRenderedFrames,
			              systemFrameSkip);

		systemSetTitle(buffer);
	}
}

void systemSetTitle(const char *title)
{
	if (theApp.m_pMainWnd != NULL)
	{
		AfxGetApp()->m_pMainWnd->SetWindowText(title);
	}
}

// time
u32 systemGetClock()
{
	return timeGetTime();
}

void systemFrame(int rate)
{
	if (theApp.altAviRecordMethod && theApp.aviRecording)
	{
		if (theApp.aviRecorder)
		{
			if (!theApp.aviRecorder->IsSoundAdded())
			{
				WAVEFORMATEX wfx;
				memset(&wfx, 0, sizeof(wfx));
				wfx.wFormatTag		= WAVE_FORMAT_PCM;
				wfx.nChannels		= 2;
				wfx.nSamplesPerSec	= 44100 / soundQuality;
				wfx.wBitsPerSample	= 16;
				wfx.nBlockAlign		= (wfx.wBitsPerSample / 8) * wfx.nChannels;
				wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
				wfx.cbSize = 0;
				theApp.aviRecorder->SetSoundFormat(&wfx);
			}
			theApp.aviRecorder->AddSound((u8 *)soundFrameSound, soundFrameSoundWritten * 2);
		}
	}

	// stupid updates :(
	struct EmulatedSystem &emu = (theApp.cartridgeType == 0) ? GBASystem : GBSystem;
	theApp.emulator.frameCount = emu.frameCount;
	theApp.emulator.lagCount   = emu.lagCount;
	theApp.emulator.lagged	   = emu.lagged;
	theApp.emulator.laggedLast = emu.laggedLast;

	extern int quitAfterTime;					// from VBA.cpp
	void VBAMovieStop(bool8 suppress_message);	// from ../movie.cpp
	if (quitAfterTime >= 0 && theApp.emulator.frameCount == quitAfterTime)
	{
		VBAMovieStop(true);
		AfxPostQuitMessage(0);
	}

	u32 time = systemGetClock();

	// change the sound speed, or set it to normal - must always do this or it won't get reset after a change, but that's OK
	// because it's inexpensive
	if (theApp.sound)
		theApp.sound->setSpeed(
		    theApp.throttle != 100 && !speedup && !theApp.winPauseNextFrame && synchronize && !theApp.accuratePitchThrottle &&
		    !theApp.useOldSync ? (float)theApp.throttle / 100.0f : 1.0f);

//  bool deadSound = !theApp.sound->isPlaying();

	// if a throttle speed is set and we're not allowed to change the sound frequency to achieve it,
	// sleep for a certain amount each time we get here to approximate the necessary slowdown
	if (((theApp.accuratePitchThrottle || !theApp.sound || !synchronize)
	     && (theApp.throttle != 100 || !synchronize))
	    || theApp.throttle < 6 && !theApp.winPauseNextFrame)
	{
		/// FIXME: this is still a horrible way of achieving a certain frame time
		///        (look at what Snes9x does - it's complicated but much much better)

		static float sleepAmt = 0.0f; // variable to smooth out the sleeping amount so it doesn't oscillate so fast
//	  if(!theApp.wasPaused) {
		if (!speedup)
		{
			u32 diff = time - theApp.throttleLastTime;
			if (theApp.wasPaused)
				diff = 0;

			int target = (100000 / (rate * theApp.throttle));
			int d	   = (target - diff);

			if (d > 1000) // added to avoid 500-day waits for vba to start emulating.
				d = 1000;  // I suspect most users aren't that patient, and would find 1 second to be a more reasonable delay.

			sleepAmt = 0.8f * sleepAmt + 0.2f * (float)d;
			if (d - sleepAmt <= 1.5f && d - sleepAmt >= -1.5f)
				d = (int)(sleepAmt);

			if (d > 0)
			{
				Sleep(d);
			}
		}
		theApp.throttleLastTime = systemGetClock();
		//}
		//else
		//{
		// Sleep(100);
		//}
	}

	if (theApp.emulator.frameCount % 10 == 0)
	{
		if (theApp.rewindMemory)
		{
			if (++theApp.rewindCounter >= (theApp.rewindTimer))
			{
				theApp.rewindSaveNeeded = true;
				theApp.rewindCounter	= 0;
			}
		}
		if (systemSaveUpdateCounter)
		{
			if (--systemSaveUpdateCounter <= SYSTEM_SAVE_NOT_UPDATED)
			{
				((MainWnd *)theApp.m_pMainWnd)->writeBatteryFile();
				systemSaveUpdateCounter = SYSTEM_SAVE_NOT_UPDATED;
			}
		}
	}

	theApp.wasPaused = false;
///  theApp.autoFrameSkipLastTime = time;
}

// input
int systemGetSensorX()
{
	return theApp.sensorX;
}

int systemGetSensorY()
{
	return theApp.sensorY;
}

// sound

bool systemSoundInit()
{
	if (theApp.sound)
		delete theApp.sound;
	
	extern ISound *newDirectSound();
	theApp.sound = newDirectSound();
	return theApp.sound->init();
}

void systemSoundShutdown()
{
	if (theApp.sound)
		delete theApp.sound;
	theApp.sound = NULL;
}

void systemSoundPause()
{
	if (theApp.sound)
		theApp.sound->pause();
}

void systemSoundResume()
{
	if (theApp.sound)
		theApp.sound->resume();
}

void systemSoundReset()
{
	if (theApp.sound)
		theApp.sound->reset();
}

void systemWriteDataToSoundBuffer()
{
	if (theApp.sound)
		theApp.sound->write();
}

bool systemCanChangeSoundQuality()
{
	return true;
}

bool systemSetSoundQuality(int quality)
{
	if (theApp.cartridgeType == 0)
		soundSetQuality(quality);
	else
		gbSoundSetQuality(quality);

	return true;
}

// emulated

void systemGbBorderOn()
{
	if (emulating && theApp.cartridgeType == 1 && gbBorderOn)
	{
		theApp.updateWindowSize(theApp.videoOption);
	}
}

bool systemIsRunningGBA()
{
	return(theApp.cartridgeType == 0);
}

bool systemIsPaused()
{
	return theApp.paused;
}

void systemSetPause(bool pause)
{
	if (pause)
	{
		theApp.wasPaused	 = true;
		theApp.paused		 = true;
		theApp.speedupToggle = false;
		soundPause();
	}
	else
	{
		theApp.wasPaused = false;
		theApp.paused	 = false;
		soundResume();
	}

	systemRefreshScreen();;
}

bool systemPauseOnFrame()
{
	if (theApp.winPauseNextFrame)
	{
		if (!theApp.nextframeAccountForLag || theApp.nextframeAccountForLag && !theApp.emulator.lagged)
		{
			theApp.winPauseNextFrame = false;
			theApp.paused = true;
		}
		else
		{
			theApp.winPauseNextFrame = true;
			return false;
		}

		return true;
	}
	return false;
}

//////////////////////////////////////////////
// ultility

extern void toolsLog(const char *);

void log(const char *msg, ...)
{
	CString buffer;
	va_list valist;

	va_start(valist, msg);
	buffer.FormatV(msg, valist);

	toolsLog(buffer);

	va_end(valist);
}
