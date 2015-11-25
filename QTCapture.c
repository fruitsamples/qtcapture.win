//////////
//
//	File:		QTCapture.c
//
//	Contains:	Capturing QuickTime movies with the Sequence Grabber.
//				Based on HackTV sample code.
//
//	Written by:	Tim Monroe
//
//	Copyright:	� 2001 by Apple Computer, Inc., all rights reserved.
//
//	Change History (most recent first):
//
//	   <3>	 	09/13/01	rtm		added video bottleneck code
//	   <2>	 	04/06/01	rtm		added monitor window refresh using SGGrabPict
//	   <1>	 	04/04/01	rtm		first file, using HackTV sources; Carbonized code
//
//////////

//////////
//
// header files
//
//////////

#include "QTCapture.h"


//////////
//
// global variables
//
//////////

// Sequence Grabber info
SeqGrabComponent				gSeqGrabber = NULL;
SGChannel						gVideoChannel = 0;
SGChannel						gSoundChannel = 0;

// monitor window info
DialogPtr						gMonitor = NULL;
PicHandle						gMonitorPICT = NULL;
Rect							gActiveVideoRect;

// monitor window sizes
Boolean							gFullSize = false;
Boolean							gHalfSize = true;
Boolean							gQuarterSize = false;

// state variables
Boolean							gRecordVideo = true;
Boolean							gRecordSound = true;
Boolean							gSplitTracks = false;

#if TARGET_OS_MAC
extern							gAppInForeground;
extern							gHasNewDialogCalls;
#endif


//////////
//
// QTCap_Init
// Initialize movie capture.
//
//////////

ComponentResult QTCap_Init (void)
{
	ComponentResult				myErr = noErr;

	// open the sequence grabber component
	gSeqGrabber = OpenDefaultComponent(SeqGrabComponentType, 0);
	if (gSeqGrabber == NULL) {
		myErr = cantOpenHandler;
		goto bail;
	}
	
	// open the monitor window
	gMonitor = GetNewDialog(kMonitorDLOGID, NULL, (WindowPtr)-1L);
	if (gMonitor == NULL) {
		myErr = memFullErr;
		goto bail;
	}
	
	SetPortDialogPort(gMonitor);
	MacMoveWindow(GetDialogWindow(gMonitor), 10, 30 + GetMBarHeight(), 0);

	// initialize the sequence grabber
	myErr = SGInitialize(gSeqGrabber);
	if (myErr == noErr) {
		// configure the sequence grabber component
		myErr = SGSetGWorld(gSeqGrabber, GetDialogPort(gMonitor), NULL);
		if (myErr != noErr)
			goto bail;
		
		// create a video channel
		myErr = SGNewChannel(gSeqGrabber, VideoMediaType, &gVideoChannel);
		if ((gVideoChannel != NULL) && (myErr == noErr)) {
			short		myWidth;
			short		myHeight;
			Rect		myRect;
			
			myErr = SGGetSrcVideoBounds(gVideoChannel, &gActiveVideoRect);
			if (myErr == noErr) {
				myWidth = (gActiveVideoRect.right - gActiveVideoRect.left) / 2;
				myHeight = (gActiveVideoRect.bottom - gActiveVideoRect.top) / 2;
				SizeWindow(GetDialogWindow(gMonitor), myWidth, myHeight, false);
			}
			
			myErr = SGSetChannelUsage(gVideoChannel, seqGrabPreview | seqGrabRecord | seqGrabPlayDuringRecord);
			if (myErr == noErr) {
				GetPortBounds(GetDialogPort(gMonitor), &myRect);
				myErr = SGSetChannelBounds(gVideoChannel, &myRect);
			}
			
			// if an error occurred while configuring video channel, dispose of it
			if (myErr != noErr) {
				SGDisposeChannel(gSeqGrabber, gVideoChannel);
				gVideoChannel = NULL;
			}
		}
		
		// create a sound channel
		myErr = SGNewChannel(gSeqGrabber, SoundMediaType, &gSoundChannel);
		if ((gSoundChannel != NULL) && (myErr == noErr)) {
			Handle		myRates = NULL;
		
			myErr = SGSetChannelUsage(gSoundChannel, seqGrabPreview | seqGrabRecord);
			if (myErr == noErr) {
				// set the volume low to prevent feedback when we start the preview
				// (in case the mic is anywhere near the speaker)
				myErr = SGSetChannelVolume(gSoundChannel, 0x0010);
			}
			
			// add some sample rates to the Sound settings dialog box Rate pop-up menu
			myRates = NewHandleClear(5 * sizeof(Fixed));
			if (myRates != NULL) {
				*((long *)(*myRates) + 0) = Long2Fix(8000);		// add 8kHz rate
				*((long *)(*myRates) + 1) = Long2Fix(11025);	// add 11kHz rate
				*((long *)(*myRates) + 2) = Long2Fix(16000);	// add 16kHz rate
				*((long *)(*myRates) + 3) = Long2Fix(22050);	// add 22kHz rate
				*((long *)(*myRates) + 4) = Long2Fix(32000);	// add 32kHz rate
				SGSetAdditionalSoundRates(gSoundChannel, myRates);

				DisposeHandle(myRates);
			}	

			// if an error occurred while configuring sound channel, dispose of it
			if (myErr != noErr) {
				SGDisposeChannel(gSeqGrabber, gSoundChannel);
				gSoundChannel = NULL;
			}
		}
	}
	
	// display the monitor window
	MacShowWindow(GetDialogWindow(gMonitor));		

	// start previewing
	if (myErr == noErr)
		myErr = SGStartPreview(gSeqGrabber);
	
bail:
	// if an error occurred, clean up
	if (myErr != noErr)
		QTCap_Stop();
	
	return(myErr);
}


//////////
//
// QTCap_Stop
// Shut down movie capture.
//
//////////

void QTCap_Stop (void)
{
	if (gSeqGrabber != NULL) {
		SGStop(gSeqGrabber);
		CloseComponent(gSeqGrabber);
		gSeqGrabber = NULL;
	}	
	
	if (gMonitor != NULL) {
		DisposeDialog(gMonitor);
		gMonitor = NULL;
	}
}


//////////
//
// QTCap_SetTrackFile
// Prompt the user for the file in which to save a track's data.
//
//////////

static ComponentResult QTCap_SetTrackFile (SGChannel theChannel, char *thePrompt, char *theDefaultName)
{
	FSSpec					myFile;
	Boolean					myIsSelected = false;
	Boolean					myIsReplacing = false;	
	StringPtr 				myPrompt = QTUtils_ConvertCToPascalString(thePrompt);
	StringPtr 				myFileName = QTUtils_ConvertCToPascalString(theDefaultName);
	SGOutput				myOutput;
	AliasHandle				myAliasHandle = NULL;
	OSErr					myErr = noErr;
	
	// prompt the user for new file name
	QTFrame_PutFile(myPrompt, myFileName, &myFile, &myIsSelected, &myIsReplacing);
	myErr = myIsSelected ? noErr : userCanceledErr;
	if (myErr != noErr)
		goto bail;
		
	myErr = QTNewAlias(&myFile, &myAliasHandle, true);
	if (myErr != noErr)
		goto bail;
		
	// create an output from this file
	myErr = SGNewOutput(gSeqGrabber, (Handle)myAliasHandle, rAliasType, seqGrabToDisk, &myOutput);
	if (myErr != noErr)
		goto bail;

	// associate this output with the specified channel
	myErr = SGSetChannelOutput(gSeqGrabber, theChannel, myOutput);

bail:
	free(myPrompt);
	free(myFileName);

	if (myAliasHandle != NULL)
		DisposeHandle((Handle)myAliasHandle);
		
	return(myErr);
}


//////////
//
// QTCap_Record
// Record the video and audio data into one or more files.
//
//////////

void QTCap_Record (void)
{
	FSSpec						myFile;
	Boolean						myIsSelected = false;
	Boolean						myIsReplacing = false;	
	StringPtr 					myPrompt = QTUtils_ConvertCToPascalString(kCapSavePrompt);
	StringPtr 					myFileName = QTUtils_ConvertCToPascalString(kCapSaveMovieFileName);
	long						myFlags = createMovieFileDontOpenFile | createMovieFileDontCreateMovie | createMovieFileDontCreateResFile;
	ComponentResult				myErr = noErr;
	
	// stop everything while the dialogs are up
	SGStop(gSeqGrabber);

	// prompt the user for new file name
	QTFrame_PutFile(myPrompt, myFileName, &myFile, &myIsSelected, &myIsReplacing);
	myErr = myIsSelected ? noErr : userCanceledErr;
	if (myErr != noErr)
		goto bail;

	// delete any existing the movie file, if the user so instructs
	if (myIsReplacing)
		DeleteMovieFile(&myFile);
	
	myErr = SGSetDataOutput(gSeqGrabber, &myFile, seqGrabToDisk);
	if (myErr != noErr)
		goto bail;

	// ask for separate video and sound track files, if requested
	if ((gSoundChannel != NULL) && gRecordSound && (gVideoChannel != NULL) && gRecordVideo && gSplitTracks) {
		myErr = QTCap_SetTrackFile(gVideoChannel, kVideoSavePrompt, kVideoSaveMovieFileName);
		if (myErr != noErr)
			goto bail;
	
		myErr = QTCap_SetTrackFile(gSoundChannel, kSoundSavePrompt, kSoundSaveMovieFileName);
		if (myErr != noErr)
			goto bail;
	}

	// if not recording sound or video, then disable those channels
	if ((gSoundChannel != NULL) && !gRecordSound)
		SGSetChannelUsage(gSoundChannel, 0);
		
	if ((gVideoChannel != NULL) && !gRecordVideo)
		SGSetChannelUsage(gVideoChannel, 0);

	// attempt to recover the preview area obscured by dialogs
#if TARGET_OS_WIN32
	UpdatePort(gMonitor);
#endif
	SGUpdate(gSeqGrabber, 0);

	// create a movie file for the destination movie
	myErr = CreateMovieFile(&myFile, sigMoviePlayer, smSystemScript, myFlags, NULL, NULL);
	if (myErr != noErr)
		goto bail;
	
	FlushEvents(mDownMask + mUpMask, 0);
	
	// record until the user clicks the mouse button
	myErr = SGStartRecord(gSeqGrabber);
	if (myErr != noErr)
		goto bail;
		
	while (!Button() && (myErr == noErr))
		myErr = SGIdle(gSeqGrabber);

	// if we recorded until we ran out of space, then allow SGStop to be called to write the movie resource;
	// the assumption here is that the data output filled up but the disk has enough free space left to
	// write the movie resource
	if (!((myErr == dskFulErr) || (myErr != eofErr)))
		goto bail;

	// stop the recording that's currently happening
	myErr = SGStop(gSeqGrabber);
 	SGStartPreview(gSeqGrabber);
	
bail:
	free(myPrompt);
	free(myFileName);

	if (myErr == noErr)
		return;
		
	SGPause(gSeqGrabber, false);
	SGStartPreview(gSeqGrabber);
}


//////////
//
// QTCap_SGModalFilterProc
// Handle events while a Sequence Grabber settings dialog box is displayed.
//
// The theRefCon parameter is expected to be the DialogPtr of the monitor window.
//
//////////

#if TARGET_OS_MAC
static PASCAL_RTN Boolean QTCap_SGModalFilterProc (DialogPtr theDialog, const EventRecord *theEvent, short *theItemHit, long theRefCon)
{
	Boolean				myEventHandled = false;
	WindowPtr			myWindow = NULL;
	RgnHandle			myWindowRgn = NULL;
	GrafPtr				mySavedPort;
	Rect				myRect;
	DialogPtr			myMonitor = (DialogPtr)theRefCon;
	
	switch (theEvent->what) {
		case updateEvt:
			// find out which window needs to be updated
			myWindow = (WindowPtr)theEvent->message;
			if (myWindow == GetDialogWindow(myMonitor)) {
				// update the monitor window, using the stored picture
				GetPort(&mySavedPort);
				MacSetPort(GetWindowPort(myWindow));

#if TARGET_API_MAC_CARBON
				GetPortBounds(GetDialogPort(myMonitor), &myRect);
#else
				myRect = myWindow->portRect;
#endif

				// draw the saved monitor picture into the monitor window
				if (gMonitorPICT != NULL)
					DrawPicture(gMonitorPICT, &myRect);
					
				// clear the update region
				BeginUpdate(myWindow);
				EndUpdate(myWindow);
				
				MacSetPort(mySavedPort);
				myEventHandled = true;
			} else if ((myWindow != NULL) && (myWindow != GetDialogWindow(theDialog))) {
				// update the specified window, if it's behind the modal dialog box
				QTFrame_HandleEvent((EventRecord *)theEvent);
				myEventHandled = false;		// so sayeth IM
			}
			break;
			
		case nullEvent:
			// do idle-time processing for all open windows in our window list
			if (gAppInForeground) 
				QTFrame_IdleMovieWindows();

			myEventHandled = false;
			break;
			
		default:
			myEventHandled = false;
			break;
	}
	
	// let the OS's standard filter proc handle the event, if it hasn't already been handled
	if (gHasNewDialogCalls && (myEventHandled == false))
		myEventHandled = StdFilterProc(theDialog, (EventRecord *)theEvent, theItemHit);
	
	return(myEventHandled);
}
#endif


//////////
//
// QTCap_GetVideoSettings
// Display the video settings dialog box and get the new settings.
//
//////////

void QTCap_GetVideoSettings (void)
{
	Rect				myNewActiveVideoRect;
	short				myWidth, myHeight;
	GrafPtr				mySavedPort;
	SGModalFilterUPP	myFilterUPP = NULL;
	Rect				myRect;
	ComponentResult		myErr = noErr;
	
	// get our current state
	GetPort(&mySavedPort);
	
	// pause previewing
	SGPause(gSeqGrabber, true);
	
	// display the video setting dialog box
	myErr = QTCap_GetChannelSettings(gVideoChannel);
	if (myErr != noErr)
		goto bail;
		
	// retrieve the user's choices
	SGGetSrcVideoBounds(gVideoChannel, &myNewActiveVideoRect);

	// set up our port
	SetPortDialogPort(gMonitor);
	
	// has our active rectangle changed?
	// if so, it's because our video standard changed (e.g., NTSC to PAL) and we need to adjust our monitor window
	if (!MacEqualRect(&gActiveVideoRect, &myNewActiveVideoRect)) {
		short			myDivisor = 1;		// assume gFullSize
		
		if (gQuarterSize)
			myDivisor = 4;
		else if (gHalfSize)
			myDivisor = 2;
			
		myWidth = (myNewActiveVideoRect.right - myNewActiveVideoRect.left) / myDivisor;
		myHeight = (myNewActiveVideoRect.bottom - myNewActiveVideoRect.top) / myDivisor;
		
		gActiveVideoRect = myNewActiveVideoRect;
		SizeWindow(GetDialogWindow(gMonitor), myWidth, myHeight, false);
		
		GetPortBounds(GetDialogPort(gMonitor), &myRect);
		SGSetChannelBounds(gVideoChannel, &myRect);
	}

bail:
	MacSetPort(mySavedPort);
	
#if !TARGET_OS_MAC
	// this is necessary, for now, to get the grab to start again after the dialog goes away;
	// for some reason the video destRect never gets reset to point back to the monitor window
	SGSetChannelBounds(gVideoChannel, &(gMonitor->portRect));
#endif

	// restart previewing
	SGPause(gSeqGrabber, false);
}


//////////
//
// QTCap_GetSoundSettings
// Display the sound settings dialog box.
//
//////////

void QTCap_GetSoundSettings (void)
{
	QTCap_GetChannelSettings(gSoundChannel);
}


//////////
//
// QTCap_GetChannelSettings
// Display the settings dialog box for the specified channel.
//
//////////

static ComponentResult QTCap_GetChannelSettings (SGChannel theChannel)
{
	SGModalFilterUPP	myFilterUPP = NULL;
	ComponentResult		myErr = noErr;

	// get rid of any existing monitor picture
	if (gMonitorPICT != NULL) {
		KillPicture(gMonitorPICT);
		gMonitorPICT = NULL;
	}
	
	// get the picture currently in the monitor window
	SGGrabPict(gSeqGrabber, &gMonitorPICT, NULL, 0, grabPictOffScreen);
	
	// display the settings dialog box
#if TARGET_OS_MAC
	myFilterUPP = NewSGModalFilterUPP(QTCap_SGModalFilterProc);
#endif

	myErr = SGSettingsDialog(gSeqGrabber, theChannel, 0, NULL, 0L, myFilterUPP, (long)gMonitor);
	
#if TARGET_OS_MAC
	DisposeSGModalFilterUPP(myFilterUPP);
#endif
	
	// get rid of the monitor picture
	if (gMonitorPICT != NULL) {
		KillPicture(gMonitorPICT);
		gMonitorPICT = NULL;
	}
		
	return(myErr);
}


//////////
//
// QTCap_ResizeMonitorWindow
// Resize the monitor window to the specified size.
//
//////////

void QTCap_ResizeMonitorWindow (short theDivisor)
{
	Rect				myRect;
	short				myWidth, myHeight;
	GrafPtr				mySavedPort;
	ComponentResult		myErr = noErr;

	// calculate the new width and height
	myWidth = (gActiveVideoRect.right - gActiveVideoRect.left) / theDivisor;
	myHeight = (gActiveVideoRect.bottom - gActiveVideoRect.top) / theDivisor;
	
	gQuarterSize = (theDivisor == 4);
	gHalfSize = (theDivisor == 2);
	gFullSize = (theDivisor == 1);
	
	// resize the monitor window
	GetPort(&mySavedPort);
	SetPortDialogPort(gMonitor);
	
	SGPause(gSeqGrabber, true);
	
	SizeWindow(GetDialogWindow(gMonitor), myWidth, myHeight, false);
	
	GetPortBounds(GetDialogPort(gMonitor), &myRect);
	SGSetChannelBounds(gVideoChannel, &myRect);

	MacSetPort(mySavedPort);
	SGPause(gSeqGrabber, false);
}


