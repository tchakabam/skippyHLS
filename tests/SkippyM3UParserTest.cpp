
#include <iostream>
#include <fstream>
#include <glib-object.h>

#include <skippyHLS/SkippyM3UParser.hpp>

#define LOG(...) g_message(__VA_ARGS__)

#define ASSERT(expr) g_assert(expr)

static std::string get_content_from_file(std::string path)
{
	std::ifstream file;
	char *buffer;
	int length;

	// Open file
	file.open(path);
	if (!file.is_open()) {
		LOG ("File could not be opened: %s", path.c_str());
		ASSERT (file.is_open());
		return "";
	}
	LOG ("File is open");
	// Get file length and allocate buffer
	file.seekg(0, std::ios::end);
	length = file.tellg();
	LOG ("File length is %d bytes", (int) length);
	buffer = new char[length];
	// Seek to beginning, read & close
	file.seekg(0, std::ios::beg);
	file.read(buffer, length);
	file.close();
	// Return content as string
	std::string content(buffer);
	return content;
}

static void test_parse_fixture_with_14_items()
{
	std::string uri = "tests/fixture14.m3u8";
	std::string playlist = get_content_from_file(uri);

	LOG ("Dump input test file: \n%s\n", playlist.c_str());

	// Testing parser
	SkippyM3UParser p;
	SkippyM3UPlaylist list = p.parse(uri, playlist);
	SkippyM3UPlaylist::iterator it = list.begin();

	LOG ("List length is %d", (int) list.size());

	ASSERT (list.size() == 14);

	LOG ("Target duration: %lu, Total duration: %lu", list.targetDuration, list.totalDuration);

	ASSERT (list.targetDuration == 10000000000);
	ASSERT (list.totalDuration == 110835651712);

	int i = 0;
	while(it != list.end()) {
		SkippyM3UItem item = *it++;

		LOG ("Item %u, start: %lu, end: %lu, duration: %lu", item.index, item.start, item.end, item.duration);

		ASSERT (item.index == i);

		switch (i) {
		case 0:
			ASSERT (item.duration == 1985272064);
			break;
		case 1:
			ASSERT (item.start == 1985272064);
			break;
		case 2:
			ASSERT (item.url == "https://ec-hls-media.soundcloud.com/media/79411/159240/5gg7H2T1t4tg.128.mp3?f10880d39085a94a0418a7e168b03d52f1af9dc5c031765e8337271e0af994fc7d4ec3878c95fb9e3ea5a614bc92616f82d2edc3b4c8ee5fec60a8192c5ab5a63ecd7d6bc875e232d68f11896070fa910284386185449c9c");
			ASSERT (item.end == 9952482304);
			break;
		}

		i++;
	}
}

int
main (int argc, char **argv)
{
	test_parse_fixture_with_14_items();

	LOG ("All test assertions passed");

	return 0;
}