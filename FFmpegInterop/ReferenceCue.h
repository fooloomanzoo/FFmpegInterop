#pragma once
#include<pch.h>
using namespace Platform;
using namespace Windows::Foundation;
using namespace Windows::Media::Core;

namespace FFmpegInterop {
	ref class ReferenceCue : IMediaCue
	{
	internal:
		ReferenceCue()
		{

		}
	public:
		virtual ~ReferenceCue()
		{
			CueRef = nullptr;
		}
	public:
		virtual property String^ Id;

		virtual property TimeSpan Duration;

		virtual property TimeSpan StartTime;

		property IMediaCue^ CueRef;
	};
}




