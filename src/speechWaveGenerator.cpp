/*
Copyright 2013 Michael Curran <mick@nvaccess.org>.
    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License version 2.1, as published by
    the Free Software Foundation.
    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
This license can be found at:
http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html
*/

#define _USE_MATH_DEFINES

#include <cassert>
#include <cmath>
#include <cstdlib>
#include "debug.h"
#include "utils.h"
#include "speechWaveGenerator.h"

using namespace std;

const double PITWO=M_PI*2;

class NoiseGenerator {
	private:
	double lastValue;

	public:
	NoiseGenerator(): lastValue(0.0) {};

	double getNext() {
		lastValue=((double)rand()/RAND_MAX)+0.75*lastValue;
		return lastValue;
	}

};

class FrequencyGenerator {
	private:
	int sampleRate;
	double lastCyclePos;

	public:
	FrequencyGenerator(int sr): sampleRate(sr), lastCyclePos(0) {}

	double getNext(double frequency) {
		double cyclePos=fmod((frequency/sampleRate)+lastCyclePos,1);
		lastCyclePos=cyclePos;
		return cyclePos;
	}

};

class VoiceGenerator {
	private:
	FrequencyGenerator pitchGen;
	FrequencyGenerator vibratoGen;
	NoiseGenerator aspirationGen;

	public:
	bool glottisOpen;
	VoiceGenerator(int sr): pitchGen(sr), vibratoGen(sr), aspirationGen(), glottisOpen(false) {};

	double getNext(const speechPlayer_frame_t* frame) {
		double vibrato=(sin(vibratoGen.getNext(frame->vibratoSpeed)*PITWO)*0.06*frame->vibratoPitchOffset)+1;
		double voice=pitchGen.getNext(frame->voicePitch*vibrato);
		double aspiration=aspirationGen.getNext();
		double turbulence=aspiration*frame->voiceTurbulenceAmplitude;
		glottisOpen=voice>=frame->glottalOpenQuotient;
		if(glottisOpen) {
			turbulence*=0.1;
		}
		voice=(voice*2)-1;
		voice+=turbulence;
		return (voice*frame->voiceAmplitude)+(aspiration*frame->aspirationAmplitude);
	}

};

class Resonator {
	private:
	//raw parameters
	int sampleRate;
	double frequency;
	double bandwidth;
	bool anti;
	//calculated parameters
	bool setOnce;
	double a, b, c;
	//Memory
	double p1, p2;

	public:
	Resonator(int sampleRate, bool anti=false) {
		this->sampleRate=sampleRate;
		this->anti=anti;
		this->setOnce=false;
		this->p1=0;
		this->p2=0;
	}

	void setParams(double frequency, double bandwidth) {
		if(!setOnce||(frequency!=this->frequency)||(bandwidth!=this->bandwidth)) {
			this->frequency=frequency;
			this->bandwidth=bandwidth;
			double r=exp(-M_PI/sampleRate*bandwidth);
			c=-(r*r);
			b=r*cos(PITWO/sampleRate*-frequency)*2.0;
			a=1.0-b-c;
			if(anti&&frequency!=0) {
				a=1.0/a;
				c*=-a;
				b*=-a;
			}
		}
		this->setOnce=true;
	}

	double resonate(double in, double frequency, double bandwidth) {
		setParams(frequency,bandwidth);
		double out=a*in+b*p1+c*p2;
		p2=p1;
		p1=anti?in:out;
		return out;
	}

};

class CascadeFormantGenerator { 
	private:
	int sampleRate;
	Resonator r1, r2, r3, r4, r5, r6, rN0, rNP;

	public:
	CascadeFormantGenerator(int sr): sampleRate(sr), r1(sr), r2(sr), r3(sr), r4(sr), r5(sr), r6(sr), rN0(sr,true), rNP(sr) {};

	double getNext(const speechPlayer_frame_t* frame, bool glottisOpen, double input) {
		input/=2.0;
		double output=input;
		if(glottisOpen) {
			output=calculateValueAtFadePosition(output,r1.resonate(output,frame->cf1+frame->dcf1,frame->cb1+frame->dcb1),frame->ca1);
		} else {
			output=calculateValueAtFadePosition(output,r1.resonate(output,frame->cf1,frame->cb1),frame->ca1);
		}
		output=calculateValueAtFadePosition(output,r2.resonate(output,frame->cf2,frame->cb2),frame->ca2);
		output=calculateValueAtFadePosition(output,r3.resonate(output,frame->cf3,frame->cb3),frame->ca3);
		output=calculateValueAtFadePosition(output,r4.resonate(output,frame->cf4,frame->cb4),frame->ca4);
		output=calculateValueAtFadePosition(output,r5.resonate(output,frame->cf5,frame->cb5),frame->ca5);
		output=calculateValueAtFadePosition(output,r6.resonate(output,frame->cf6,frame->cb6),frame->ca6);
		output=calculateValueAtFadePosition(output,rN0.resonate(output,frame->cfN0,frame->cbN0),frame->caN0);
		output=calculateValueAtFadePosition(output,rNP.resonate(output,frame->cfNP,frame->cbNP),frame->caNP);
		return output;
	}

};

class ParallelFormantGenerator { 
	private:
	int sampleRate;
	Resonator r1, r2, r3, r4, r5, r6;

	public:
	ParallelFormantGenerator(int sr): sampleRate(sr), r1(sr), r2(sr), r3(sr), r4(sr), r5(sr), r6(sr) {};

	double getNext(const speechPlayer_frame_t* frame, double input) {
		input/=2.0;
		double output=input;
		output+=calculateValueAtFadePosition(input,r1.resonate(input,frame->pf1,frame->pb1)-input,frame->pa1);
		output+=calculateValueAtFadePosition(input,r2.resonate(input,frame->pf2,frame->pb2)-input,frame->pa2);
		output+=calculateValueAtFadePosition(input,r3.resonate(input,frame->pf3,frame->pb3)-input,frame->pa3);
		output+=calculateValueAtFadePosition(input,r4.resonate(input,frame->pf4,frame->pb4)-input,frame->pa4);
		output+=calculateValueAtFadePosition(input,r5.resonate(input,frame->pf5,frame->pb5)-input,frame->pa5);
		output+=calculateValueAtFadePosition(input,r6.resonate(input,frame->pf6,frame->pb6)-input,frame->pa6);
		return output;
	}

};

class SpeechWaveGeneratorImpl: public SpeechWaveGenerator {
	private:
	int sampleRate;
	VoiceGenerator voiceGenerator;
	NoiseGenerator fricGenerator;
	CascadeFormantGenerator cascade;
	ParallelFormantGenerator parallel;
	FrameManager* frameManager;

	public:
	SpeechWaveGeneratorImpl(int sr): sampleRate(sr), voiceGenerator(sr), fricGenerator(), cascade(sr), parallel(sr), frameManager(NULL) {
	}

	void generate(const int sampleCount, sample* sampleBuf) {
		if(!frameManager) return; 
		double val=0;
		for(int i=0;i<sampleCount;++i) {
			const speechPlayer_frame_t* frame=frameManager->getCurrentFrame();
			if(frame) {
				double voice=voiceGenerator.getNext(frame);
				double cascadeOut=cascade.getNext(frame,voiceGenerator.glottisOpen,voice);
				double fric=fricGenerator.getNext()*frame->fricationAmplitude;
				double parallelOut=parallel.getNext(frame,fric);
				double out=(cascadeOut+parallelOut)*frame->gain;
				sampleBuf[i].value=max(min(out*4000,32000),-32000);
			} else {
				sampleBuf[i].value=0;
			}
		}
	}

	void setFrameManager(FrameManager* frameManager) {
		this->frameManager=frameManager;
	}

};

SpeechWaveGenerator* SpeechWaveGenerator::create(int sampleRate) {return new SpeechWaveGeneratorImpl(sampleRate); }