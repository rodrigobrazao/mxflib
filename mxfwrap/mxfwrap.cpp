/*! \file	mxfwrap.cpp
 *	\brief	Basic MXF essence wrapping utility
 *
 *	\version $Id: mxfwrap.cpp,v 1.45 2011/01/10 11:00:50 matt-beard Exp $
 *
 */
/*
 *	Copyright (c) 2003, Matt Beard
 *	Portions Copyright (c) 2003-5, Metaglue Corporation
 *
 *	This software is provided 'as-is', without any express or implied warranty.
 *	In no event will the authors be held liable for any damages arising from
 *	the use of this software.
 *
 *	Permission is granted to anyone to use this software for any purpose,
 *	including commercial applications, and to alter it and redistribute it
 *	freely, subject to the following restrictions:
 *
 *	  1. The origin of this software must not be misrepresented; you must
 *	     not claim that you wrote the original software. If you use this
 *	     software in a product, an acknowledgment in the product
 *	     documentation would be appreciated but is not required.
 *	
 *	  2. Altered source versions must be plainly marked as such, and must
 *	     not be misrepresented as being the original software.
 *	
 *	  3. This notice may not be removed or altered from any source
 *	     distribution.
 */

#include "mxflib/mxflib.h"
using namespace mxflib;

#include "parseoptions.h"

#include <stdio.h>
#include <iostream>

using namespace std;


//! Debug flag for MXFLib
static bool DebugMode = false;


// Debug and error messages
#include <stdarg.h>


#ifdef MXFLIB_DEBUG
//! Display a general debug message
void mxflib::debug(const char *Fmt, ...)
{
	if(!DebugMode) return;

	va_list args;

	va_start(args, Fmt);
	vprintf(Fmt, args);
	va_end(args);
}
#endif // MXFLIB_DEBUG

//! Display a warning message
void mxflib::warning(const char *Fmt, ...)
{
	va_list args;

	va_start(args, Fmt);
	printf("Warning: ");
	vprintf(Fmt, args);
	va_end(args);
}

//! Display an error message
void mxflib::error(const char *Fmt, ...)
{
	va_list args;

	va_start(args, Fmt);
	printf("ERROR: ");
	vprintf(Fmt, args);
	va_end(args);
}

//! Choose the best wrapping option for a given input file
EssenceParser::WrappingConfigPtr ChooseWrapping(FileParserPtr &FParser, ProcessOptions &Opt);

// OP Qualifier manipulators: ClearStream, SetStream, SetUniTrack, SetMultiTrack
void ClearStream(UL &theUL);
void SetStream(UL &theUL);
void SetUniTrack(UL &theUL);
void SetMultiTrack(UL &theUL);



// DICTIONARY PROCESSING
// to use a fixed dict.xml, convert it to dict.h using dictconvert.exe
// then #define COMPILED_DICT 


#ifdef COMPILED_DICT
const bool DefaultCompiledDict = true;
#else
const bool DefaultCompiledDict = false;
#endif // COMPILED_DICT

// include the autogenerated dictionary
#include "mxflib/dict.h"

// DM Dictionaries - see process.h

// DictLoaded is a flag to make sure we only load the dictionaries once
static bool DictLoaded=false;			



//! Should we pause before exit?
int PauseBeforeExit = 0;



//! Class for printing next file name
class FilenameHandler : public NewFileHandler
{
public:
	//! Receive notification of a new file about to be opened
	/*! \param FileName - reference to a std::string containing the name of the file about to be opened - <b>may be changed by this function if required</b>
	 */
	virtual void NewFile(std::string &FileName)
	{
		if(FileExists(FileName.c_str()))
		{
			printf("    Essence File: %s\n", FileName.c_str());
		}
	}
};



// Declare main process function
int main_process(int argc, char *argv[]);

//! Do the main processing and pause if required
int main(int argc, char *argv[]) 
{ 
	int Ret = main_process(argc, argv);

	if( PauseBeforeExit>0 ) PauseForInput();

	return Ret;
}

//! Do the main processing (less any pause before exit)
int main_process(int argc, char *argv[])
{
	printf( "MXFlib File Wrapper\n\n" );

	// ProcessOptions - struct to contain all the options and flags obtained from the client
	// declared and constructed in Process.h
	ProcessOptions Opt;


	// Parse command line options and exit on error
	PauseBeforeExit = ParseOptions(argc, argv, &Opt);
	if( PauseBeforeExit<0 ) return 1;

	DebugMode = Opt.DebugMode;

	// Enable FastClipWrap mode - don't do this if random access not available of the output medium
	SetFastClipWrap(true);

	// DICTIONARY PROCESSING
		// Load the dictionaries
		if(!DictLoaded) // only load the dictionary once
		{
			int DictLoadResult = 1;

			if( !DefaultCompiledDict )
			{
				if( Opt.OverrideDictionary ) DictLoadResult=LoadDictionary( DictData );
				else if( !Opt.OrthodoxDict.empty() ) DictLoadResult=LoadDictionary( (char *)Opt.OrthodoxDict.c_str() );
				else DictLoadResult=LoadDictionary( "dict.xml" );
			}
			else
			{
				if( !Opt.OverrideDictionary ) DictLoadResult=LoadDictionary( DictData );
				else if( !Opt.OrthodoxDict.empty() ) DictLoadResult=LoadDictionary( (char *)Opt.OrthodoxDict.c_str() );
				else DictLoadResult=LoadDictionary( "dict.xml" );
			}
			if( DictLoadResult ) error( "Orthodox dictionary failed to load\n" );


			// load any DM Dictionaries
			DMFileList::iterator dd_it = Opt.DMDicts.begin();
			while( !DictLoadResult && dd_it != Opt.DMDicts.end() )
			{
				DictLoadResult=LoadDictionary( (char *)(*dd_it).c_str() );
				if( DictLoadResult ) error( "DM Dictionary ", (char *)(*dd_it).c_str(), " failed to load\n" );
				dd_it++;
			}

			if( DictLoadResult ) return 1;

			// only load the dictionary once
			DictLoaded=true;
		}
	// END DICTIONARY PROCESSING

	EssenceParser::WrappingConfigList WrappingList;
	EssenceParser::WrappingConfigList::iterator WrappingList_it;
	// The edit rate for all tracks in this file
	Rational EditRate;

	// Essence sources with file package index for each input file (or list of files)
	EssenceSourcePair InFileSource[ProcessOptions::MaxInFiles];
	
	// The start and end edit unit for each source, -1,-1 if all is to be wrapped
	std::pair<Length, Length> InFileRange[ProcessOptions::MaxInFiles];

	// Audio Demuxers
	AudioDemuxPtr AudioDemuxer[ProcessOptions::MaxInFiles];


	// Identify the wrapping options
	// DRAGONS: Not flexible yet
	std::string InitialFile;							//!< The name of the first file if processing a single list of source files
	int i;
	int InCount = Opt.InFileGangSize * Opt.InFileGangCount;
	int iFilePackage = 0;
	int OutNum = 0;
	for(i=0; i< InCount; i++)
	{
		FileParserPtr FParser = new FileParser(Opt.InFilename[i]);

		// Wrapping config to use
		EssenceParser::WrappingConfigPtr WCP = ChooseWrapping(FParser, Opt);

		if(!WCP) return 3;


		// If we are processing a single list of files add a handler to print each name
		if((InCount == 1) && (FParser->IsFileList()))
		{
			InitialFile = FParser->FileName();
			FParser->SetNewFileHandler(new FilenameHandler);
		}

		// Set the wrapping options
		FParser->Use(WCP->Stream, WCP->WrapOpt);
		
		// Install the descriptor in the source
		FParser->SetDescriptor(WCP->EssenceDescriptor);

		// Edit rate for this source
		EditRate = WCP->EditRate;

		// DRAGONS: Once we have set the edit rate for the first file we force it on the rest
		Opt.ForceEditRate = EditRate;

		// Ensure the essence descriptor reflects the new wrapping
		// Will be overridden for Avid
		WCP->EssenceDescriptor->SetValue(EssenceContainer_UL, DataChunk(16,WCP->WrapOpt->WrappingUL->GetValue()));


		// Add a locator if this essence is extarnal
		if(FParser->IsExternal())
		{
			WCP->IsExternal = true;

			MDObjectPtr NetLocator = new MDObject( NetworkLocator_UL );
			WCP->EssenceDescriptor->AddRef(Locators_UL, NetLocator);

			NetLocator->SetString(URLString_UL, FParser->GetRawFileName());
		}

		/* Audio Demultiplexing */
		if(Opt.AudioLimit && WCP->EssenceDescriptor->IsA(WaveAudioDescriptor_UL) && (!WCP->EssenceDescriptor->IsA(AES3PCMDescriptor_UL)))
		{
			unsigned int Count = WCP->EssenceDescriptor->GetUInt("ChannelCount");

			// DRAGONS: Note that we "demux" if the number of bits need to be changed, even if the number of channels is OK
			if((Count > Opt.AudioLimit) || (Opt.AudioBits != 0))
			{
				MDObjectPtr OriginalDescriptor = WCP->EssenceDescriptor;

				// Inform the user of the demultiplexed wrapping
				printf("\nAudio demultiplexing of file \"%s\" :\n", Opt.InFilename[i]);

				if(WCP->WrapOpt->ThisWrapType == WrappingOption::Frame)
				{
					// Frame-wrapping can use a single demux for all channels as the buffers will get freed after each frame
					int DemuxIndex = OutNum;
					AudioDemuxer[OutNum] = new AudioDemux(FParser->GetEssenceSource(WCP->Stream), Count, (unsigned int) WCP->EssenceDescriptor->GetInt("QuantizationBits"),0);
					if(Opt.AudioBits != 0) AudioDemuxer[OutNum]->SetOutputBitSize(Opt.AudioBits);

					unsigned int j;
					for(j=0; j<Count; j += Opt.AudioLimit)
					{
						// Work out how many channels this source
						unsigned int ChanCount = Opt.AudioLimit;
						if((j + ChanCount) > Count) ChanCount = Count - j;

						// Build some descriptive text to add to the format details
						char Buffer[128];
						if(Opt.AudioBits == 0)
						{
							if(ChanCount == 1)
								sprintf(Buffer, " (Channel %u)", j + 1);
							else
								sprintf(Buffer, " (Channels %u to %u)", j + 1, j + ChanCount);
						}
						else
						{
							if(ChanCount == 1)
								sprintf(Buffer, " (Channel %u, %u-bit)", j + 1, Opt.AudioBits);
							else
								sprintf(Buffer, " (Channels %u to %u, %u-bit)", j + 1, j + ChanCount, Opt.AudioBits);
						}

						// Add this wrapping option for each channel (with a single-channel copy of the essence descriptor)
						EssenceParser::WrappingConfigPtr WrapCfg = new EssenceParser::WrappingConfig;

						// Copy the contents (into a duplicate WrappingConfig, also making a duplicate WrappingOption)
						*WrapCfg = *WCP;
						WrapCfg->WrapOpt = new WrappingOption;
						*(WrapCfg->WrapOpt) = *(WCP->WrapOpt);

						WrapCfg->EssenceDescriptor = OriginalDescriptor->MakeCopy();
						WrapCfg->EssenceDescriptor->SetInt("ChannelCount", ChanCount);
						
						// Install this new essence descriptor in the source
						FParser->SetDescriptor(WrapCfg->EssenceDescriptor);
		
						// Update the descriptor with the new quantization bits
						if(Opt.AudioBits) WrapCfg->EssenceDescriptor->SetInt("QuantizationBits", Opt.AudioBits);

						WrapCfg->WrapOpt->Description += Buffer;
						WrappingList.push_back(WrapCfg);

						// Increase the apparent number of input files as it now looks like one file per demux-source
						InFileSource[OutNum].first = iFilePackage;
						InFileSource[OutNum].second = AudioDemuxer[DemuxIndex]->GetSource(j, ChanCount);
						OutNum++;
						Opt.InFileGangSize++;
			
						printf("    %s\n", WrapCfg->WrapOpt->Description.c_str());
					}
				}
				else
				{
					/* Clip-wrapping (and possibly other wrappings) will need one demux object per channel to prevent "live" buffers filling memory */

					unsigned int j;
					for(j=0; j<Count; j += Opt.AudioLimit)
					{
						// Work out how many channels this source
						unsigned int ChanCount = Opt.AudioLimit;
						if((j + ChanCount) > Count) ChanCount = Count - j;

						// Build some descriptive text to add to the format details
						char Buffer[128];
						if(Opt.AudioBits == 0)
						{
							if(ChanCount == 1)
								sprintf(Buffer, " (Channel %u)", j + 1);
							else
								sprintf(Buffer, " (Channels %u to %u)", j + 1, j + ChanCount);
						}
						else
						{
							if(ChanCount == 1)
								sprintf(Buffer, " (Channel %u, %u-bit)", j + 1, Opt.AudioBits);
							else
								sprintf(Buffer, " (Channels %u to %u, %u-bit)", j + 1, j + ChanCount, Opt.AudioBits);
						}

						FileParserPtr ThisFParser;
						EssenceParser::WrappingConfigPtr WrapCfg;
						if(j==0)
						{
							ThisFParser = FParser;
							// Add this wrapping option for each channel (with a single-channel copy of the essence descriptor)
							WrapCfg = new EssenceParser::WrappingConfig;
							
							// Copy the contents (into a duplicate WrappingConfig, also making a duplicate WrappingOption)
							*WrapCfg = *WCP;
							WrapCfg->WrapOpt = new WrappingOption;
							*(WrapCfg->WrapOpt) = *(WCP->WrapOpt);

							WrapCfg->EssenceDescriptor = OriginalDescriptor->MakeCopy();
							WrapCfg->EssenceDescriptor->SetInt("ChannelCount", ChanCount);

							// Install this new essence descriptor in the source
							FParser->SetDescriptor(WrapCfg->EssenceDescriptor);
						}
						else
						{
							ThisFParser = new FileParser(Opt.InFilename[i]);
							WrapCfg = ChooseWrapping(ThisFParser, Opt);

							// Set the wrapping options
							ThisFParser->Use(WrapCfg->Stream, WrapCfg->WrapOpt);

							// Ensure the essence descriptor reflects the new wrapping
							// Will be overridden for Avid
							WrapCfg->EssenceDescriptor->SetValue(EssenceContainer_UL, DataChunk(16,WrapCfg->WrapOpt->WrappingUL->GetValue()));

							WrapCfg->EssenceDescriptor->SetInt("ChannelCount", ChanCount);

							// Install the essence descriptor in the new source
							ThisFParser->SetDescriptor(WrapCfg->EssenceDescriptor);
						}

						// Make a demux object with a single output
						AudioDemuxer[OutNum] = new AudioDemux(ThisFParser->GetEssenceSource(WrapCfg->Stream), Count, WrapCfg->EssenceDescriptor->GetInt("QuantizationBits"),0);
						if(Opt.AudioBits != 0) AudioDemuxer[OutNum]->SetOutputBitSize(Opt.AudioBits);

						// Increase the apparent number of input files as it now looks like one file per demux-source
						InFileSource[OutNum].first = iFilePackage;
						InFileSource[OutNum].second = AudioDemuxer[OutNum]->GetSource(j, ChanCount);
						OutNum++;
						Opt.InFileGangSize++;

						// Update the descriptor with the new quantization bits (after using the old value to build the demux!)
						if(Opt.AudioBits) WrapCfg->EssenceDescriptor->SetInt("QuantizationBits", Opt.AudioBits);

						// Add this wrapping
						WrapCfg->WrapOpt->Description += Buffer;
						WrappingList.push_back(WrapCfg);

						printf("    %s\n", WrapCfg->WrapOpt->Description.c_str());
					}
				}
			}
		}
		else
		{
			// Add this wrapping option
			WrappingList.push_back(WCP);

			// Record the essence source for this source file or files
			InFileSource[OutNum].first = iFilePackage;
			InFileSource[OutNum].second = FParser->GetEssenceSource(WCP->Stream);
			OutNum++;

			// Inform the user of the chosen wrapping
			printf("\nSelected wrapping for file \"%s\" : %s\n", Opt.InFilename[i], WCP->WrapOpt->Description.c_str());
		
			// Set up any sub-streams
			if(Opt.IncludeSubstreams)
			{
				EssenceParser::WrappingConfigList::iterator subit = WCP->SubStreams.begin();
				while(subit != WCP->SubStreams.end())
				{
					printf("  SubStream: %s\n", (*subit)->WrapOpt->Description.c_str());

					// Add this sub-stream
					InFileSource[OutNum].first = iFilePackage;
					InFileSource[OutNum].second = (FParser->GetSubSource((*subit)->Stream));
					OutNum++;
					Opt.InFileGangSize++;

					// Ensure the essence descriptor reflects the new wrapping
					(*subit)->EssenceDescriptor->SetValue(EssenceContainer_UL, DataChunk(16,(*subit)->WrapOpt->WrappingUL->GetValue()));

					// Add this wrapping option to the data to wrap
					WrappingList.push_back(*subit);

					subit++;
				}
			}

			// Up the file package index (unless frame grouping)
			if(!Opt.FrameGroup) iFilePackage++;
		}
	}

	fflush(stderr);
	fflush(stdout);


	// Generate UMIDs for each file package
	UMIDPtr FPUMID[ProcessOptions::MaxInFiles];								//! UMIDs for each file package (internal or external)
	i = 0;				//  Essence container and track index
	WrappingList_it = WrappingList.begin();
	while(WrappingList_it != WrappingList.end())
	{
		switch((*WrappingList_it)->WrapOpt->GCEssenceType)
		{
		case 0x05: case 0x15:
			FPUMID[i] = MakeUMID(1);
			break;
		case 0x06: case 0x16:
			FPUMID[i] = MakeUMID(2);
			break;
		case 0x07: case 0x17:
			FPUMID[i] = MakeUMID(3);
			break;
		case 0x18: default:
			FPUMID[i] = MakeUMID(4);
			break;
		}

		WrappingList_it++;
		i++;
	}

	// Set any OP qualifiers
	if(!Opt.OPAtom)
	{
		if((Opt.FrameGroup) || (WrappingList.size() == 1))
		{
			// FIXME: This is wrong!!
			SetUniTrack(Opt.OPUL);
			SetStream(Opt.OPUL);
		}
		else
		{
			// FIXME: This is wrong!!
			SetMultiTrack(Opt.OPUL);
			if(Opt.StreamMode) SetStream(Opt.OPUL); else ClearStream(Opt.OPUL);
		}
	}

	// Generate a UMID for the Material Package
	UMIDPtr MPUMID = MakeUMID( 0x0d ); // mixed type


	// DRAGONS: there is only ONE which is used by all files in an OP-Atom set
	// FIXME: if we ever start building multiple nonOP-Atom files at one time, need to modify the UMID each time

	int OutFileNum;
	for(OutFileNum=0; OutFileNum < Opt.OutFileCount ; OutFileNum++)
	{
		// Open the output file
		MXFFilePtr Out = new MXFFile;
		if(!Out->OpenNew(Opt.OutFilename[OutFileNum]))
		{
			error("Can't open output file \"%s\"\n", Opt.OutFilename[OutFileNum]);
			return 5;
		}

		printf( "\nProcessing output file \"%s\"\n", Opt.OutFilename[OutFileNum]);

		if(InitialFile.size()) printf("    Essence File: %s\n", InitialFile.c_str());

		Length dur = Process(
					OutFileNum, 
					Out,
					&Opt,
					WrappingList, 
					InFileSource, 
					EditRate,
					MPUMID, 
					FPUMID,
					NULL
				);

		printf( "Duration = %s edit units\n", UInt64toString( dur ).c_str() );

		// Close the file - all done!
		Out->Close();


	}

	printf("\nDone\n");

	return 0;
}


// OP Qualifier manipulators: ClearStream, SetStream, SetUniTrack, SetMultiTrack
void ClearStream(UL &theUL)
{
	UInt8 Buffer[16];

	memcpy(Buffer, theUL.GetValue(), 16);

	if(Buffer[12] > 3) 
	{
		warning("ClearStream() called on specialized OP UL\n");
		return;
	}

	Buffer[14] |= 0x04;

	theUL.Set(Buffer);
}

void SetStream(UL &theUL)
{
	UInt8 Buffer[16];

	memcpy(Buffer, theUL.GetValue(), 16);

	if(Buffer[12] > 3) 
	{
		warning("SetStream() called on specialized OP UL\n");
		return;
	}

	Buffer[14] &= ~0x04;

	theUL.Set(Buffer);
}


void SetUniTrack(UL &theUL)
{
	UInt8 Buffer[16];

	memcpy(Buffer, theUL.GetValue(), 16);

	if(Buffer[12] > 3) 
	{
		warning("SetUniTrack() called on specialized OP UL\n");
		return;
	}

	Buffer[14] &= ~0x08;

	theUL.Set(Buffer);
}

void SetMultiTrack(UL &theUL)
{
	UInt8 Buffer[16];

	memcpy(Buffer, theUL.GetValue(), 16);

	if(Buffer[12] > 3) 
	{
		warning("SetMultiTrack() called on specialized OP UL\n");
		return;
	}

	Buffer[14] |= 0x08;

	theUL.Set(Buffer);
}


//! Choose the best wrapping option for a given input file
EssenceParser::WrappingConfigPtr ChooseWrapping(FileParserPtr &FParser, ProcessOptions &Opt)
{
	// Wrapping config to use
	EssenceParser::WrappingConfigPtr WCP;

	ParserDescriptorListPtr PDList = FParser->IdentifyEssence();

	if((!PDList) || (PDList->empty()))
	{
		error("Could not identify the essence in file \"%s\"\n", FParser->FileName().c_str());
		return WCP;
	}

	// Auto wrapping selection
	if(Opt.SelectedWrappingOption < 0)
	{
		// Select the best wrapping option
		if(Opt.FrameGroup) WCP = FParser->SelectWrappingOption(PDList, Opt.ForceEditRate, Opt.KAGSize, WrappingOption::Frame);
		else WCP = FParser->SelectWrappingOption(PDList, Opt.ForceEditRate, Opt.KAGSize);
	}
	else
	// Manual wrapping selection
	{
		EssenceParser::WrappingConfigList WCList;
		if(Opt.FrameGroup) WCList = FParser->ListWrappingOptions(PDList, Opt.ForceEditRate, WrappingOption::Frame);
		else WCList = FParser->ListWrappingOptions(PDList, Opt.ForceEditRate);

		// Ensure that there are enough wrapping options
		if(((size_t)Opt.SelectedWrappingOption) > WCList.size())
		{
			error("Wrapping option %d not available\n", Opt.SelectedWrappingOption);
			Opt.SelectedWrappingOption = 0;
		}

		// If the caller has requested a list of wrapping options, list them and exit
		if(Opt.SelectedWrappingOption == 0)
		{
			// First try and match the text - if this fails we will drop through and list the options
			if(Opt.SelectedWrappingOptionText.length())
			{
				EssenceParser::WrappingConfigList::iterator it = WCList.begin();
				while(it != WCList.end())
				{
					std::string Name = (*it)->WrapOpt->Handler->GetParserName();
					if(Name.length() && (*it)->WrapOpt->Name.length()) Name += "::" + (*it)->WrapOpt->Name;

					if(Name == Opt.SelectedWrappingOptionText)
					{
						WCP = *it;
						break;
					}

					it++;
				}

				if(!WCP) printf("\nWrapping \"%s\" not available\n", Opt.SelectedWrappingOptionText.c_str());
			}

			// The caller has requested a list of wrapping options (or entered an invalid name), list them and exit
			if(!WCP)
			{
				int nWrOpt = 0;
				printf("\nAvailable wrapping options:\n");

				EssenceParser::WrappingConfigList::iterator it = WCList.begin();
				while(it != WCList.end())
				{
					printf("  %d: %s\n", (++nWrOpt), (*it)->WrapOpt->Description.c_str());
						
					EssenceParser::WrappingConfigList::iterator subit = (*it)->SubStreams.begin();
					while(subit != (*it)->SubStreams.end())
					{
						printf("     SubStream: %s\n", (*subit)->WrapOpt->Description.c_str());
						subit++;
					}

					it++;
				}

				if(nWrOpt == 0) printf("  NONE\n");

				return WCP;
			}
		}

		// Select the nth config
		EssenceParser::WrappingConfigList::iterator it = WCList.begin();
		while(Opt.SelectedWrappingOption-- > 1) it++;
		WCP = *it;
		
		WCP->KAGSize = Opt.KAGSize;
		
		FParser->SelectWrappingOption(WCP);
	}

	if(!WCP)
	{
		error("Could not identify a wrapping mode for the essence in file \"%s\"\n", FParser->FileName().c_str());
	}

	return WCP;
}

