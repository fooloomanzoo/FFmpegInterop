#pragma once

#include <string>
#include <codecvt>

#include "CompressedSampleProvider.h"
#include "StreamInfo.h"
#include "NativeBufferFactory.h"
#include <ReferenceCue.h>

using namespace Windows::UI::Core;

namespace FFmpegInterop
{
	ref class SubtitleProvider abstract : CompressedSampleProvider
	{

	internal:

		SubtitleProvider(FFmpegReader^ reader,
			AVFormatContext* avFormatCtx,
			AVCodecContext* avCodecCtx,
			FFmpegInteropConfig^ config,
			int index,
			TimedMetadataKind timedMetadataKind,
			CoreDispatcher^ dispatcher)
			: CompressedSampleProvider(reader, avFormatCtx, avCodecCtx, config, index)
		{
			this->timedMetadataKind = timedMetadataKind;
			this->dispatcher = dispatcher;
		}

		property TimedMetadataTrack^ SubtitleTrack;

		virtual HRESULT Initialize() override
		{
			InitializeNameLanguageCodec();

			SubtitleTrack = ref new TimedMetadataTrack(Name, Language, timedMetadataKind);
			SubtitleTrack->Label = Name != nullptr ? Name : Language;

			if (!m_config->IsExternalSubtitleParser)
			{
				SubtitleTrack->CueEntered += ref new Windows::Foundation::TypedEventHandler<Windows::Media::Core::TimedMetadataTrack ^, Windows::Media::Core::MediaCueEventArgs ^>(this, &FFmpegInterop::SubtitleProvider::OnCueEntered);
				SubtitleTrack->TrackFailed += ref new Windows::Foundation::TypedEventHandler<Windows::Media::Core::TimedMetadataTrack ^, Windows::Media::Core::TimedMetadataTrackFailedEventArgs ^>(this, &FFmpegInterop::SubtitleProvider::OnTrackFailed);
			}

			return S_OK;
		}

	internal:
		virtual void NotifyVideoFrameSize(int width, int height, double aspectRatio)
		{
		}

		virtual IMediaCue^ CreateCue(AVPacket* packet, TimeSpan* position, TimeSpan *duration) = 0;

		virtual void QueuePacket(AVPacket *packet) override
		{
			if (m_isEnabled)
			{
				TimeSpan position;
				TimeSpan duration;
				bool isDurationFixed = false;

				position.Duration = LONGLONG(av_q2d(m_pAvStream->time_base) * 10000000 * packet->pts) - m_startOffset;
				duration.Duration = LONGLONG(av_q2d(m_pAvStream->time_base) * 10000000 * packet->duration);

				auto cue = CreateCue(packet, &position, &duration);
				if (cue && position.Duration >= 0)
				{
					if (duration.Duration <= 0)
					{
						duration.Duration = InfiniteDuration;
					}

					cue->StartTime = position;
					cue->Duration = duration;
					AddCue(cue);

					isPreviousCueInfiniteDuration = duration.Duration >= InfiniteDuration;
				}
			}
			av_packet_free(&packet);
		}

		// convert UTF-8 string to wstring
		std::wstring utf8_to_wstring(const std::string& str)
		{
			std::wstring_convert<std::codecvt_utf8<wchar_t>> myconv;
			return myconv.from_bytes(str);
		}

		Platform::String ^ convertFromString(const std::wstring & input)
		{
			return ref new Platform::String(input.c_str(), (unsigned int)input.length());
		}

	private:

		void AddCue(IMediaCue^ cue)
		{
			mutex.lock();
			try
			{
				if (Windows::Foundation::Metadata::ApiInformation::IsApiContractPresent("Windows.Phone.PhoneContract", 1, 0))
				{
					/*This is a fix only to work around a bug in windows phones: when 2 different cues have the exact same start position and length, the runtime panics and throws an exception
					The problem has only been observed in external subtitles so far, and only on phones. Might also be present on ARM64 devices*/
					bool individualCue = true;
					if (this->timedMetadataKind == TimedMetadataKind::Subtitle) {
						for (int i = SubtitleTrack->Cues->Size - 1; i >= 0; i--)
						{
							auto existingSub = (TimedTextCue^)SubtitleTrack->Cues->GetAt(i);

							if (existingSub->StartTime.Duration == cue->StartTime.Duration && existingSub->Duration.Duration == cue->Duration.Duration)
							{
								individualCue = false;
								auto timedTextCue = (TimedTextCue^)cue;
								for each(auto l in timedTextCue->Lines)
								{
									existingSub->Lines->Append(l);
								}
							}

							break;
						}
					}
					if (individualCue)
					{
						DispatchCueToTrack(cue);
					}
				}
				else
				{
					DispatchCueToTrack(cue);
				}

			}

			catch (...)
			{
				OutputDebugString(L"Failed to add subtitle cue.");
			}
			mutex.unlock();
		}

		void DispatchCueToTrack(IMediaCue^ cue)
		{
			if (m_config->IsExternalSubtitleParser)
			{
				SubtitleTrack->AddCue(cue);
			}
			else if (isPreviousCueInfiniteDuration)
			{
				pendingRefCues.push_back(ref new ReferenceCue(cue));
				StartTimer();
			}
			else
			{
				pendingCues.push_back(cue);
				StartTimer();
			}
		}

		void OnRefCueEntered(TimedMetadataTrack ^sender, MediaCueEventArgs ^args)
		{
			mutex.lock();
			try {
				//remove all cues from subtitle track
				while (SubtitleTrack->Cues->Size > 0)
				{
					SubtitleTrack->RemoveCue(SubtitleTrack->Cues->GetAt(0));
				}

				auto refCue = static_cast<ReferenceCue^>(args->Cue);
				SubtitleTrack->AddCue(refCue->CueRef);
				referenceTrack->RemoveCue(refCue);
			}
			catch (...)
			{
			}
			mutex.unlock();
		}

		void OnCueEntered(Windows::Media::Core::TimedMetadataTrack ^sender, Windows::Media::Core::MediaCueEventArgs ^args)
		{
			mutex.lock();
			try
			{
				//cleanup old cues to free memory
				std::vector<IMediaCue^> remove;
				for each (auto cue in SubtitleTrack->Cues)
				{
					if (cue->StartTime.Duration + cue->Duration.Duration < args->Cue->StartTime.Duration)
					{
						remove.push_back(cue);
					}
				}

				for each (auto cue in remove)
				{
					SubtitleTrack->RemoveCue(cue);
				}
			}
			catch (...)
			{
				OutputDebugString(L"Failed to cleanup old cues.");
			}
			mutex.unlock();
		}

		void StartTimer()
		{
			dispatcher->RunAsync(Windows::UI::Core::CoreDispatcherPriority::Normal,
				ref new Windows::UI::Core::DispatchedHandler([this]
			{
				if (timer == nullptr)
				{
					timer = ref new Windows::UI::Xaml::DispatcherTimer();
					TimeSpan interval;
					interval.Duration = 10000;
					timer->Interval = interval;
					timer->Tick += ref new Windows::Foundation::EventHandler<Platform::Object ^>(this, &FFmpegInterop::SubtitleProvider::OnTick);
				}
				timer->Start();
			}));
		}

		void OnTick(Platform::Object ^sender, Platform::Object ^args)
		{
			mutex.lock();
			try
			{
				for each (auto cue in pendingCues)
				{
					SubtitleTrack->AddCue(cue);
				}

				if (pendingRefCues.size() > 0)
				{
					EnsureRefTrackInitialized();

					for each (auto cue in pendingRefCues)
					{
						referenceTrack->AddCue(cue);
					}
				}
			}
			catch (...)
			{
				OutputDebugString(L"Failed to add pending subtitle cues.");
			}

			pendingCues.clear();
			pendingRefCues.clear();
			timer->Stop();
			mutex.unlock();
		}

		void EnsureRefTrackInitialized()
		{
			if (referenceTrack == nullptr)
			{
				referenceTrack = ref new TimedMetadataTrack("ReferenceTrack_" + Name, "", TimedMetadataKind::Custom);
				referenceTrack->CueEntered += ref new Windows::Foundation::TypedEventHandler<Windows::Media::Core::TimedMetadataTrack ^, Windows::Media::Core::MediaCueEventArgs ^>(this, &FFmpegInterop::SubtitleProvider::OnRefCueEntered);

				SubtitleTrack->PlaybackItem->TimedMetadataTracksChanged += ref new Windows::Foundation::TypedEventHandler<Windows::Media::Playback::MediaPlaybackItem ^, Windows::Foundation::Collections::IVectorChangedEventArgs ^>(this, &FFmpegInterop::SubtitleProvider::OnTimedMetadataTracksChanged);
				SubtitleTrack->PlaybackItem->Source->ExternalTimedMetadataTracks->Append(referenceTrack);
			}
		}

		void OnTimedMetadataTracksChanged(Windows::Media::Playback::MediaPlaybackItem ^sender, Windows::Foundation::Collections::IVectorChangedEventArgs ^args)
		{
			// enable ref track
			if (args->CollectionChange == CollectionChange::ItemInserted &&
				sender->TimedMetadataTracks->GetAt(args->Index) == referenceTrack)
			{
				SubtitleTrack->PlaybackItem->TimedMetadataTracks->SetPresentationMode(
					args->Index, Windows::Media::Playback::TimedMetadataTrackPresentationMode::Hidden);
			}
		}

		void OnTrackFailed(TimedMetadataTrack ^sender, TimedMetadataTrackFailedEventArgs ^args)
		{
			OutputDebugString(L"Subtitle track error.");
		}

	public:

		void Flush() override
		{
			CompressedSampleProvider::Flush();

			mutex.lock();

			try
			{
				while (SubtitleTrack->Cues->Size > 0)
				{
					SubtitleTrack->RemoveCue(SubtitleTrack->Cues->GetAt(0));
				}

				if (referenceTrack != nullptr)
				{
					while (referenceTrack->Cues->Size > 0)
					{
						referenceTrack->RemoveCue(referenceTrack->Cues->GetAt(0));
					}
				}

				pendingCues.clear();
				pendingRefCues.clear();
				isPreviousCueInfiniteDuration = false;
			}
			catch (...)
			{
			}

			mutex.unlock();
		}

	private:

		std::mutex mutex;
		int cueCount;
		std::vector<IMediaCue^> pendingCues;
		std::vector<IMediaCue^> pendingRefCues;
		TimedMetadataKind timedMetadataKind;
		Windows::UI::Core::CoreDispatcher^ dispatcher;
		Windows::UI::Xaml::DispatcherTimer^ timer;
		bool isPreviousCueInfiniteDuration;
		TimedMetadataTrack^ referenceTrack;
		const long long InfiniteDuration = ((long long)0xFFFFFFFF) * 10000;

};
}



