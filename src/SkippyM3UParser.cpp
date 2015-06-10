/*
 *
 *  Created on: May 25, 2011
 *      Author: Stephan Hesse <disparat@gmail.com>, <stephan@soundcloud.com>
 */

#define UNIT_SECONDS 1000000000L // 10^9 (Nanoseconds)

#define USE_GLIB_TOKENIZER FALSE
#define USE_CPP11_TOKENIZER FALSE
#define ENABLE_DEBUG_LOG FALSE

#include <sstream>
#include <iostream>
#include <cmath>
#include <algorithm>

#if USE_CPP11_TOKENIZER
#include <regex>
#endif

#include <string.h> // For strlen

#include <glib-object.h>

#include "skippyHLS/SkippyM3UParser.hpp"

#if ENABLE_DEBUG_LOG
  #define LOG(...) g_message(__VA_ARGS__)
#else
  #define LOG(...)
#endif

using namespace std;

typedef vector<string> Tokens;

#if USE_GLIB_TOKENIZER

Tokens glib_split(string s) {
  auto tokens;
  gchar** c_tokens = g_str_tokenize_and_fold (s.c_str(), NULL, NULL);
  if (!c_tokens) {
    LOG ("Failed to tokenize string");
    return;
  }
  for (gchar** list = c_tokens;*list != NULL;list++) {
    tokens.push_back(string(*list));
  }
  g_strfreev (c_tokens);
  return tokens;
}

#endif

#if USE_CPP11_TOKENIZER

Tokens regex_split(string s) {
  auto tokens, item ("");
  regex ex ("\\W"); // matches all non-word chars
  regex_token_iterator<string::iterator> it(begin(s), end(s), ex, -1), end;
  for_each(it, end, [](auto it){
    item = *it;
    if (item.length() == 0) {
      continue;
    }
    transform(begin(item), end(item), begin(item), ::tolower);
    tokens.push_back(item);
  });
  return tokens;
}

#endif

void custom_split_push_token (string s, Tokens& tokens, size_t index, size_t wordLength)
{
  string item;
  item = s.substr(index, wordLength);
  transform(begin(item), end(item), begin(item), ::tolower);
  tokens.push_back(item);
  //LOG ("Token: %s", item.c_str());
}

Tokens custom_split(string s) {
  Tokens tokens;
  string delims ("# -.,:");
  string::iterator it;
  int lastMatch = 0,
      wordLength = 0;

  // Iterate and push what we find
  for (int i=0;i<s.length();i++) {
    for (it = begin(delims);it != end(delims);it++) {
      // We have a match
      if (s[i] == *it) {
        //LOG ("Match: %c in %s", *it, s.c_str());
        wordLength = i - lastMatch;
        // Skip zero length tokens
        if (wordLength > 0) {
          custom_split_push_token (s, tokens, lastMatch, wordLength);
          lastMatch = i + 1;
          break;
        } else {
          lastMatch = i + 1;
          continue;
        }
      }
    }
  }
  if (lastMatch < s.length()) {
    // Push the very last (or only) word
    custom_split_push_token (s, tokens, lastMatch, string::npos);
  }

  return tokens;
}

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
  stringstream in(playlist);

  // Output playlist
  SkippyM3UPlaylist outputPlaylist(uri);

  size_t cnt = 0;
  while ( ++cnt ){

	  //we have to check if the line was EOF now and break in case
	  if ( in.eof() ) {
	  	LOG ("End of file");
	  	break;
	  }

	  //read next line from file
	  getline(in, line);

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
#if USE_GLIB_TOKENIZER
  tokens = glib_split (line);
#elif USE_CPP11_TOKENIZER
  tokens = regex_split (line);
#else
  tokens = custom_split (line);
#endif
  tokenIt = begin(tokens);
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

		if (line.find_first_of("\r", 0) != string::npos && line.length() >= 2) {
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