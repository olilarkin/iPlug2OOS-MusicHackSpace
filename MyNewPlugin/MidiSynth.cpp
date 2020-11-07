/*
 ==============================================================================
 
 This file is part of the iPlug 2 library. Copyright (C) the iPlug 2 developers. 
 
 See LICENSE.txt for  more info.
 
 ==============================================================================
*/

#include "MidiSynth.h"

MidiSynth::MidiSynth(EPolyMode polyMode, int blockSize, int nUnisonVoices)
: mGranularity(blockSize)
, mUnisonVoices(nUnisonVoices)
{
  mSustainedNotes.reserve(128);
  mHeldKeys.reserve(128);
  
  for (int i=0; i<128; i++)
  {
    mVelocityLUT[i] = i;
    mAfterTouchLUT[i] = i;
  }
}

MidiSynth::~MidiSynth()
{
  mVS.Empty(true);
}

bool MidiSynth::ProcessBlock(sample** inputs, sample** outputs, int nInputs, int nOutputs, int nFrames)
{
  assert(NVoices()); // you didn't add any voices to the synth!
  
  for (int c=0; c<nOutputs; c++) {
    memset(outputs[c], 0, nFrames * sizeof(sample) );
  }

  if (mVoicesAreActive | !mMidiQueue.Empty())
  {
    int bs = mGranularity;
    int samplesRemaining = nFrames;
    int s = 0;

    Voice* pVoice;

    while(samplesRemaining > 0)
    {
      if(samplesRemaining < bs)
        bs = samplesRemaining;

      s = nFrames - samplesRemaining;

      //TODO: here there should be a mechanism for updating "click safe" variables

      while (!mMidiQueue.Empty())
      {
        IMidiMsg& msg = mMidiQueue.Peek();

        if (msg.mOffset > s) break;

        int status = msg.StatusMsg(); // get the MIDI status byte

        switch (status)
        {
          case IMidiMsg::kNoteOn:
          case IMidiMsg::kNoteOff:
          {
            if (mPolyMode == kPolyModePoly)
              NoteOnOffPoly(msg);
            else
              NoteOnOffMono(msg);

            break;
          }
          case IMidiMsg::kPolyAftertouch:
          {
            for(int v = 0; v < NVoices(); v++)
            {
              if(mATMode == kATModePoly && GetVoice(v)->mKey == msg.NoteNumber())
              {
                GetVoice(v)->mAftertouch = mAfterTouchLUT[msg.PolyAfterTouch()] / 127.;
              }
            }

            break;
          }
          case IMidiMsg::kChannelAftertouch:
          {
            if(mATMode == kATModeChannel)
            {
              double val = mAfterTouchLUT[msg.ChannelAfterTouch()] / 127.;

              for(int v = 0; v < NVoices(); v++)
              {
                GetVoice(v)->mAftertouch = val;
              }
            }
            break;
          }
          case IMidiMsg::kPitchWheel:
          {
            mPitchBend = msg.PitchWheel();
            break;
          }
          case IMidiMsg::kControlChange:
          {
            switch (msg.ControlChangeIdx())
            {
              case IMidiMsg::kModWheel:
                mModWheel = msg.ControlChange(IMidiMsg::kModWheel);
                break;
              case IMidiMsg::kSustainOnOff:

                mSustainPedalDown = (bool) (msg.ControlChange(IMidiMsg::kSustainOnOff) >= 0.5);

                if (!mSustainPedalDown) // sustain pedal released
                {
                  // if notes are sustaining, check that they're not still held and if not then stop voice
                  if (!mSustainedNotes.empty())
                  {
                    std::vector<KeyPressInfo>::iterator susNotesItr;

                    for (susNotesItr = mSustainedNotes.begin(); susNotesItr != mSustainedNotes.end();)
                    {
                      const bool held = std::find(mHeldKeys.begin(), mHeldKeys.end(), *susNotesItr) != mHeldKeys.end();

                      if (!held)
                      {
                        StopVoicesForKey(susNotesItr->mKey);
                        susNotesItr = mSustainedNotes.erase(susNotesItr);
                      }
                      else
                        susNotesItr++;
                    }
                  }
                }

                break;
              case IMidiMsg::kAllNotesOff:
                mHeldKeys.clear();
                mSustainedNotes.clear();
                mSustainPedalDown = false;
                AllNotesOff();
                break;
              default:
                break;
            }
            break;
          }
        }

        mMidiQueue.Remove();
      }

      ProcessSlice(inputs, outputs, nInputs, nOutputs, s, bs);

      for(int v = 0; v < NVoices(); v++) // for each vs
      {
        pVoice = GetVoice(v);

        if (pVoice->GetBusy())
        {
          pVoice->ProcessSamples(inputs, outputs, nInputs, nOutputs, s, bs, mPitchBend);
        }
      }

      samplesRemaining -= bs;
      mSampleTime += bs;
    }

    bool voicesbusy = false;
    int activeCount = 0;

    for(int v = 0; v < NVoices(); v++)
    {
      bool busy = GetVoice(v)->GetBusy();
      voicesbusy |= busy;

      activeCount += (busy==true);

      mVoiceStatus[v] = busy;
    }

    mVoicesAreActive = voicesbusy;

    mMidiQueue.Flush(nFrames);
  }
  else // empty block
  {
    return true;
  }

  return false; // made some noise
}

#pragma mark - NOTE TRIGGER METHODS

void MidiSynth::NoteOnOffPoly(const IMidiMsg& msg)
{
  int status = msg.StatusMsg();
  int velocity = msg.Velocity();
  int note = msg.NoteNumber();

  if (status == IMidiMsg::kNoteOn && velocity)
  {
    const double velNorm = static_cast<double>(Clip(mVelocityLUT[velocity], 1, 127)) / 127.;

    KeyPressInfo theNote = KeyPressInfo(note, velNorm);
    
    TriggerPolyNote(theNote);
  }
  else  // Note off
  {
    std::vector<KeyPressInfo>::iterator it;
    bool erase = false;

    // REMOVE released key from held keys list. Do this even if the sustain pedal is down
    for (it = mHeldKeys.begin(); it != mHeldKeys.end(); it++)
    {
      if (it->mKey == note)
      {
        erase = true;
        break;
      }
    }

    if (erase)
      mHeldKeys.erase(it);

    if (!mSustainPedalDown)
    {
      // REMOVE released key from sustained keys list
      erase = false;

      for (it = mSustainedNotes.begin(); it != mSustainedNotes.end(); it++)
      {
        if (it->mKey == note)
        {
          erase = true;
          break;
        }
      }

      if (erase)
        mSustainedNotes.erase(it);
      
      StopVoicesForKey(note);
    }
  }
}

void MidiSynth::NoteOnOffMono(const IMidiMsg& msg)
{
  int status = msg.StatusMsg();
  int velocity = msg.Velocity();
  int note = msg.NoteNumber();

  if (status == IMidiMsg::kNoteOn && velocity)
  {
    velocity = Clip(mVelocityLUT[velocity], 1, 127);
    double velNorm = (double) velocity / 127.;

    KeyPressInfo theNote = KeyPressInfo(note, velNorm);

    if(std::find(mHeldKeys.begin(), mHeldKeys.end(), theNote) == mHeldKeys.end())
      mHeldKeys.push_back(theNote);

    // kinda stupid in mono modes only ever 1 sustained note
    mSustainedNotes.clear();
    mSustainedNotes.push_back(theNote);

    TriggerMonoNote(theNote);

    mPrevVelNorm = velNorm;
  }
  else  // Note off
  {
    std::vector<KeyPressInfo>::iterator it;
    bool erase = false;

    // REMOVE released key from held keys list. Do this even if the sustain pedal is down
    for (it = mHeldKeys.begin(); it != mHeldKeys.end(); it++)
    {
      if (it->mKey == note)
      {
        erase = true;
        break;
      }
    }

    if (erase)
      mHeldKeys.erase(it);

    // if there are still held keys...
    if(!mHeldKeys.empty())
    {
      KeyPressInfo queuedNote = mHeldKeys.back();

      if (queuedNote.mKey != GetVoice(0)->mKey)
      {
        // kinda stupid in mono modes only ever 1 sustained note
        mSustainedNotes.clear();
        mSustainedNotes.push_back(queuedNote);
        TriggerMonoNote(queuedNote);
      }
    }
    else
    {
      if (mSustainPedalDown)
      {
        KeyPressInfo queuedNote = mSustainedNotes.back();

        if (queuedNote.mKey != GetVoice(0)->mKey)
        {
          // don't need to add to sustained queue, allready in it
          TriggerMonoNote(queuedNote);
        }
      }
      else
        StopVoicesForKey(note);
    }
  }
}

inline void MidiSynth::TriggerPolyNote(KeyPressInfo keyPress)
{
  // if keyPress not found in held keys push it on
  if(std::find(mHeldKeys.begin(), mHeldKeys.end(), keyPress) == mHeldKeys.end())
    mHeldKeys.push_back(keyPress);
  
  // if keyPress not found in sustained keys push it on
  if(std::find(mSustainedNotes.begin(), mSustainedNotes.end(), keyPress) == mSustainedNotes.end())
    mSustainedNotes.push_back(keyPress);
  
  for (int uv = 0; uv < mUnisonVoices; uv++)
  {
    int v = -1;

    v = FindFreeVoice(); // or first one triggered
  
    if (v == -1) // shouldn't happen
      return;
  
    Voice* pVoice = GetVoice(v);
    
    pVoice->mStartTime = mSampleTime;
    pVoice->mKey = keyPress.mKey;
    pVoice->mStackIdx = uv;
    pVoice->mBasePitch = GetAdjustedPitch(keyPress.mKey);
    pVoice->mAftertouch = 0.;
    pVoice->Trigger(keyPress.mVelNorm, pVoice->GetBusy()); // if voice is busy it will retrigger
  }
  
  mVoicesAreActive = true;
  
  mPrevKey = keyPress.mKey;
}

void MidiSynth::TriggerMonoNote(KeyPressInfo note)
{
  for (int v = 0; v < mUnisonVoices; v++)
  {
    Voice* pVoice = GetVoice(v);

    pVoice->mKey = note.mKey;
    pVoice->mStackIdx = v;
    pVoice->mBasePitch = GetAdjustedPitch(note.mKey);
    pVoice->mAftertouch = 0.;

    const bool voiceFree = !pVoice->GetBusy();
    const bool voiceReleased = pVoice->GetReleased();

    if (voiceFree)
    {
      pVoice->Trigger(note.mVelNorm, false);
    }
    else if(mPolyMode == kPolyModeMono || voiceReleased)
    {
      pVoice->Trigger(note.mVelNorm, true);
    }
  }

  mVoicesAreActive = true;
}

void MidiSynth::SetSampleRateAndBlockSize(double sampleRate, int blockSize)
{
  Reset();

  mSampleRate = sampleRate;
  mMidiQueue.Resize(blockSize);

  for(int v = 0; v < NVoices(); v++)
  {
    GetVoice(v)->SetSampleRate(sampleRate);
  }
}
