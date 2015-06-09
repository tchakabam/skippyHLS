/*
 *
 *  Created on: May 25, 2011
 *      Author: Stephan Hesse <disparat@gmail.com>, <stephan@soundcloud.com>
 */

#include <sstream>
#include <iostream>
#include <cmath>

#include <string.h> // For strlen

#include <glib-object.h>

#include "skippyHLS/SkippyM3UParser.hpp"

#define LOG(...) g_message(__VA_ARGS__)

#define UNIT_SECONDS 1000000000L // 10^9 (Nanoseconds)

using namespace std;

// words
static const string EXT("ext");
static const string X("x");
static const string INF("inf");
static const string ID("id");
static const string EXTM3U("extm3u");
static const string EXTINF("extinf");
static const string PLAYLIST("playlist");
static const string TYPE("type");
static const string STREAM("stream");
static const string PROGRAM("program");
static const string VERSION("version");
static const string BANDWIDTH("bandwidth");
static const string RES("res");
static const string CODEC("codec");
static const string VOD("vod");
static const string EVENT("event");
static const string TARGETDURATION("targetduration");
static const string MEDIA("media");
static const string SEQUENCE("sequence");
static const string ENDLIST("endlist");

SkippyM3UParser::SkippyM3UParser()
:state (STATE_RESET)
,subState (SUBSTATE_RESET)
// Put default values here
,programId(0)
,bandwidth(0)
,index(0)
,length(0)
,position(0)
,mediaSequenceNo(0)
,targetDuration(0)
{}

SkippyM3UPlaylist SkippyM3UParser::Parse(string uri, const string& playlist)
{
  std::stringstream in(playlist);

  // Output playlist
  SkippyM3UPlaylist outputPlaylist(uri);

  //std::cout << "Initialized memory." << "\n";

  size_t cnt = 0;
  while ( ++cnt ){

	  //we have to check if the line was EOF now and break in case
	  if ( in.eof() ) {
	  	LOG ("End of file");
	  	break;
	  }

	  //read next line from file
	  std::getline(in, line);

	  // (re-)evaluate main state of parser
	  EvalState();

	  // Parses the current line into tokens
	  // and updates the parser members
	  ReadLine();

	  // Updates the output playlist after every line
	  Update(outputPlaylist);
  }

  return outputPlaylist;
}

void SkippyM3UParser::MetaTokenize() {
  gchar** c_tokens = g_str_tokenize_and_fold (line.c_str(), NULL, NULL);
  if (!c_tokens) {
  	LOG ("Failed to tokenize string");
    return;
  }
  tokens.clear();
  for (gchar** list = c_tokens;*list != NULL;list++) {
    tokens.push_back(std::string(*list));
  }
  g_strfreev (c_tokens);
  tokenIt = tokens.begin();
}

void SkippyM3UParser::EvalState() {

	LOG ("Evaluating line: %s", line.c_str());

	//is it META LINE ?
	if (line.find("#EXT") == 0) { //check if its a META line and that are not already in META line state

		LOG ("Found meta line");
		state = STATE_META_LINE;

	// could it be a URL ?
	} else if(state == STATE_META_LINE && subState == SUBSTATE_INF) {

		LOG ("Assuming URL line");
		state = STATE_URL_LINE;

	//OK if nothing correct and we had a URL _before_we can just reset the parser
	} else if (state == STATE_URL_LINE) {

		LOG ("Reset");
		state = STATE_RESET;

	//ok we had a META LINE before or NOTHING or a RESET so nothing happens...
	//if we had META before we have to stay there because we wait for a URL to complete the output info for the object writer
	} else if ( state == STATE_META_LINE || state == STATE_RESET)  {

	}
}

bool SkippyM3UParser::NextToken() {
	if(tokenIt == tokens.end()) {
		return false;
	}
	token = *tokenIt++;
	return true;
}

void SkippyM3UParser::EvalSubstate() {

	// Get the very first token !
	NextToken();

	// Skip these tokens
 	if (token == EXT) {
 		NextToken();
 	}
 	if (token == X) {
 		NextToken();
 	}

	LOG ("Evaluating metaline substate from token: %s", token.c_str());

 	if (token == EXTM3U) {

 		LOG ("Start of M3U");

   	} else if ( token == EXTINF ) {

		subState = SUBSTATE_INF;

		LOG ("Sub-State to: INF");

   	} else if ( token == STREAM && NextToken()
   			&& token == INF) {

		subState = SUBSTATE_STREAM;

		LOG ("Sub-State to: STREAM");

   	} else if ( token == MEDIA && NextToken()
   			&& token == SEQUENCE && NextToken() ) {

   		mediaSequenceNo = atoi(token.c_str());

   		LOG ("Media sequence no: %u", mediaSequenceNo);

   	} else if (token == PLAYLIST && NextToken()
   			&& token == TYPE && NextToken() ) {

   		playlistType = token;

   		LOG ("Playlist type: %s", playlistType.c_str());

   	} else if (token == VERSION && NextToken()) {

   		version = atoi(token.c_str());

   		LOG ("Version is: %u", version);
   	} else if (token == TARGETDURATION && NextToken()) {

   		targetDuration = atoi(token.c_str());

   		LOG ("Target duration is: %u", targetDuration);
   	} else if (token == ENDLIST) {

   		LOG("Sub-State to: RESET (end of list)");

		subState = SUBSTATE_END;

   	} else {

   		LOG("Sub-State to: RESET (unknown token): %s", token.c_str());

   		subState = SUBSTATE_RESET;
   	}

}

void SkippyM3UParser::ReadLine() {

    //tokenize meta string -> split up into pieces seperated by non-alphanumeric chars

	switch(state) {
	case STATE_RESET:
		break;
	case STATE_URL_LINE:

		if (line.find_first_of("\r", 0) != std::string::npos && line.length() >= 2) {
			line = line.substr(0, line.length()-1);
		}
	  	url = line;
	  	state = STATE_URL_LINE;
	  	subState = SUBSTATE_RESET;
  		break;

	case STATE_META_LINE:

		// Reset the segment length (duration) field
		length = -1;

		// Tokenize the line
		MetaTokenize();

		// Evaluate the substate
	    EvalSubstate();

	    //iterate over all tokens of this line
	    while (NextToken())
	    {
			switch(subState) {
	      	case SUBSTATE_INF:
	        	if (length == -1) {
	        		length = (float) strtod(token.c_str(), NULL);
	        	} else {
	        		float decimals = ((float) strtod(token.c_str(), NULL));
	        		LOG ("Got decimals: %f (%s)", decimals, token.c_str());
	        		length += decimals / pow(10, strlen(token.c_str()));
	        	}
		        LOG ("Got INF duration: %f", length);
	        	break;
	    	case SUBSTATE_STREAM:
		        if ( token == PROGRAM && NextToken () && token == ID ) {
		         NextToken();
		         programId = atoi(token.c_str());
		        }
		        else if ( token == CODEC ) {
		         NextToken();
		         codec = token;
		        }
		        else if ( token == RES) {
		         NextToken();
		         res = token;
		        }
		        else if ( token == BANDWIDTH) {
		         NextToken();
		         bandwidth = atoi(token.c_str());
		        }
				break;
			case SUBSTATE_RESET:
			default:
	        	break;
			}
	    }
    	break;
  }
}

/*
 * the object writing machine. takes the parsing machine states synchronisly with them as input and their
 */
void SkippyM3UParser::Update(SkippyM3UPlaylist& playlist) {

	/*
	* use output of parsing machine to write to object
	*/
	switch(state) {
	case STATE_RESET:
		break;
	case STATE_URL_LINE: {
		SkippyM3UItem item;
		item.start = position;
		item.duration = length * UNIT_SECONDS;
		item.end = item.start + item.duration;
		item.url = url;
		item.encrypted = false;
		item.index = index;

	    playlist.push_back( item );

		position += item.duration;
		index++;

	    LOG ("Added item: %s", url.c_str());
	    break;
		}
	case STATE_META_LINE:
		switch (subState) {
		case SUBSTATE_END:
			playlist.bandwidthKbps = bandwidth; //kbps
			playlist.codec = codec;
			playlist.resolution = res;
			playlist.programId = programId;
			playlist.sequenceNo = mediaSequenceNo;
			playlist.targetDuration = targetDuration * UNIT_SECONDS;
			playlist.totalDuration = position;
			playlist.type = playlistType;
			break;
		default:
			break;
		}
  		break;
  	}
}