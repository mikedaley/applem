/*
 * audio_output.mm - The default output device, pulling stereo samples
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#include "audio_output.hpp"

#include <AudioToolbox/AudioToolbox.h>

namespace a2e::native {

struct AudioOutput::Impl {
  AudioComponentInstance unit = nullptr;
  Pull pull;
  bool running = false;

  static OSStatus render(void *refCon, AudioUnitRenderActionFlags *,
                         const AudioTimeStamp *, UInt32, UInt32 frames,
                         AudioBufferList *buffers) {
    Impl *self = static_cast<Impl *>(refCon);
    float *out = static_cast<float *>(buffers->mBuffers[0].mData);
    self->pull(out, frames);
    return noErr;
  }
};

AudioOutput::AudioOutput() : impl_(std::make_unique<Impl>()) {}

AudioOutput::~AudioOutput() { stop(); }

bool AudioOutput::start(double sampleRate, Pull pull) {
  stop();
  impl_->pull = std::move(pull);

  AudioComponentDescription description = {};
  description.componentType = kAudioUnitType_Output;
  description.componentSubType = kAudioUnitSubType_DefaultOutput;
  description.componentManufacturer = kAudioUnitManufacturer_Apple;
  AudioComponent component = AudioComponentFindNext(nullptr, &description);
  if (!component) return false;
  if (AudioComponentInstanceNew(component, &impl_->unit) != noErr) return false;

  AudioStreamBasicDescription format = {};
  format.mSampleRate = sampleRate;
  format.mFormatID = kAudioFormatLinearPCM;
  format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
  format.mChannelsPerFrame = 2;
  format.mBitsPerChannel = 32;
  format.mFramesPerPacket = 1;
  format.mBytesPerFrame = sizeof(float) * 2;
  format.mBytesPerPacket = format.mBytesPerFrame;

  AURenderCallbackStruct callback = {};
  callback.inputProc = &Impl::render;
  callback.inputProcRefCon = impl_.get();

  const bool ok =
      AudioUnitSetProperty(impl_->unit, kAudioUnitProperty_StreamFormat,
                           kAudioUnitScope_Input, 0, &format,
                           sizeof(format)) == noErr &&
      AudioUnitSetProperty(impl_->unit, kAudioUnitProperty_SetRenderCallback,
                           kAudioUnitScope_Input, 0, &callback,
                           sizeof(callback)) == noErr &&
      AudioUnitInitialize(impl_->unit) == noErr &&
      AudioOutputUnitStart(impl_->unit) == noErr;
  if (!ok) {
    AudioComponentInstanceDispose(impl_->unit);
    impl_->unit = nullptr;
    return false;
  }
  impl_->running = true;
  return true;
}

void AudioOutput::stop() {
  if (!impl_->unit) return;
  AudioOutputUnitStop(impl_->unit);
  AudioUnitUninitialize(impl_->unit);
  AudioComponentInstanceDispose(impl_->unit);
  impl_->unit = nullptr;
  impl_->running = false;
}

bool AudioOutput::running() const { return impl_->running; }

} // namespace a2e::native
