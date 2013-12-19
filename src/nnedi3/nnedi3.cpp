/*
**                    nnedi3 v0.9.4 for Avisynth 2.5.x
**
**   Copyright (C) 2010-2011 Kevin Stone
**
**   This program is free software; you can redistribute it and/or modify
**   it under the terms of the GNU General Public License as published by
**   the Free Software Foundation; either version 2 of the License, or
**   (at your option) any later version.
**
**   This program is distributed in the hope that it will be useful,
**   but WITHOUT ANY WARRANTY; without even the implied warranty of
**   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**   GNU General Public License for more details.
**
**   You should have received a copy of the GNU General Public License
**   along with this program; if not, write to the Free Software
**   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#include "nnedi3.h"

EXTERN_C IMAGE_DOS_HEADER __ImageBase;

int num_processors()
{
#ifdef _DEBUG
	return 1;
#else
	int pcount = 0;
	DWORD p_aff, s_aff;
	GetProcessAffinityMask(GetCurrentProcess(), &p_aff, &s_aff);
	for(; p_aff != 0; p_aff>>=1) 
		pcount += (p_aff&1);
	return pcount;
#endif
}

int roundds(const double f)
{
	if (f-floor(f) >= 0.5)
		return min((int)ceil(f),32767);
	return max((int)floor(f),-32768);
}

void shufflePreScrnL2L3(float *wf, float *rf, const int opt)
{
	for (int j=0; j<4; ++j)
		for (int k=0; k<4; ++k)
			wf[k*4+j] = rf[j*4+k];
	rf += 4*5;
	wf += 4*5;
	const int jtable[4] = { 0, 2, 1, 3 };
	for (int j=0; j<4; ++j)
	{
		for (int k=0; k<8; ++k)
			wf[k*4+j] = rf[jtable[j]*8+k];
		wf[4*8+j] = rf[4*8+jtable[j]];
	}
}

nnedi3::nnedi3(PClip _child, int _field, bool _dh, bool _Y, bool _U, bool _V, int _nsize, 
	int _nns, int _qual, int _etype, int _pscrn, int _threads, int _opt, int _fapprox, 
	IScriptEnvironment *env) : GenericVideoFilter(_child), field(_field), dh(_dh), 
	Y(_Y), U(_U), V(_V), nsize(_nsize), nns(_nns), qual(_qual), etype(_etype), pscrn(_pscrn), 
	threads(_threads), opt(_opt), fapprox(_fapprox)
{
	if (field < -2 || field > 3)
		env->ThrowError("nnedi3:  field must be set to -2, -1, 0, 1, 2, or 3!");
	if (threads < 0 || threads > 16)
		env->ThrowError("nnedi3:  threads must be between 0 and 16 inclusive!");
	if (dh && (field < -1 || field > 1))
		env->ThrowError("nnedi3:  field must be set to -1, 0, or 1 when dh=true!");
	if (nsize < 0 || nsize >= NUM_NSIZE)
		env->ThrowError("nnedi3:  nsize must be in [0,%d]!\n", NUM_NSIZE-1);
	if (nns < 0 || nns >= NUM_NNS)
		env->ThrowError("nnedi3:  nns must be in [0,%d]!\n", NUM_NNS-1);
	if (qual < 1 || qual > 2)
		env->ThrowError("nnedi3:  qual must be set to 1 or 2!\n");
	if (opt < 0 || opt > 2)
		env->ThrowError("nnedi3:  opt must be set to 0, 1, or 2!");
	if (fapprox < 0 || fapprox > 15)
		env->ThrowError("nnedi3:  fapprox must be [0,15]!\n");
	if (pscrn < 0 || pscrn > 4)
		env->ThrowError("nnedi3:  pscrn must be [0,4]!\n");
	if (etype < 0 || etype > 1)
		env->ThrowError("nnedi3:  etype must be [0,1]!\n");
	if (field == -2)
		field = child->GetParity(0) ? 3 : 2;
	else if (field == -1)
		field = child->GetParity(0) ? 1 : 0;
	if (field > 1)
	{
		vi.num_frames *= 2;
		vi.SetFPS(vi.fps_numerator*2, vi.fps_denominator);
	}
	if (dh)
		vi.height *= 2;
	vi.SetFieldBased(false);
	child->SetCacheHints(CACHE_RANGE,3);
	if (threads == 0)
		threads = num_processors();
	srcPF = new PlanarFrame();
	srcPF->createPlanar(vi.height+12,(vi.IsYV12()?(vi.height>>1):vi.height)+12,
		vi.width+64,(vi.IsRGB24()?vi.width:(vi.width>>1))+64);
	dstPF = new PlanarFrame(vi);
	if (opt == 0)
	{
		const int cpuf = srcPF->cpu;
		if (cpuf&CPU_SSE2)
			opt = 2;
		else
			opt = 1;
		char buf[512];
		sprintf(buf,"nnedi3:  auto-detected opt setting = %d (%d)\n", opt, cpuf);
		OutputDebugString(buf);
	}
	const int dims0 = 49*4+5*4+9*4;
	const int dims0new = 4*65+4*5;
	const int dims1 = nnsTable[nns]*2*(xdiaTable[nsize]*ydiaTable[nsize]+1);
	int dims1tsize = 0, dims1offset;
	for (int j=0; j<NUM_NNS; ++j)
	{
		for (int i=0; i<NUM_NSIZE; ++i)
		{
			if (i == nsize && j == nns)
				dims1offset = dims1tsize;
			dims1tsize += nnsTable[j]*2*(xdiaTable[i]*ydiaTable[i]+1)*2;
		}
	}
	weights0 = (float*)_aligned_malloc(max(dims0,dims0new)*sizeof(float),16);
	for (int i=0; i<3; ++i)
	{
		if (i < 2)
			weights1[i] = (float*)_aligned_malloc(dims1*sizeof(float),16);
		lcount[i] = (int*)_aligned_malloc(dstPF->GetHeight(i)*sizeof(int),16);
	}
	char nbuf[512];
	GetModuleFileName((HINSTANCE)&__ImageBase,nbuf,512);
	HMODULE hmod = GetModuleHandle(nbuf);
	if (!hmod)
		env->ThrowError("nnedi3:  unable to get module handle!");
	HRSRC hrsrc = FindResource(hmod,MAKEINTRESOURCE(101),_T("BINARY"));
	HGLOBAL hglob = LoadResource(hmod,hrsrc);
	LPVOID lplock = LockResource(hglob);
	DWORD dwSize = SizeofResource(hmod,hrsrc);
	if (!hmod || !hrsrc || !hglob || !lplock || 
		dwSize != (dims0+dims0new*3+dims1tsize*2)*sizeof(float))
		env->ThrowError("nnedi3:  error loading resource (%x,%x,%x,%x,%d,%d)!", hmod, 
			hrsrc, hglob, lplock, dwSize, (dims0+dims0new*3+dims1tsize*2)*sizeof(float));
	float *bdata = (float*)lplock;
	// Adjust prescreener weights
	if (pscrn >= 2) // using new prescreener
	{
		int *offt = (int*)calloc(4*64,sizeof(int));
		for (int j=0; j<4; ++j)
			for (int k=0; k<64; ++k)
				offt[j*64+k] = ((k>>3)<<5)+((j&3)<<3)+(k&7);
		const float *bdw = bdata+dims0+dims0new*(pscrn-2);
		short *ws = (short*)weights0;
		float *wf = (float*)&ws[4*64];
		double mean[4] = { 0.0, 0.0, 0.0, 0.0 };
		// Calculate mean weight of each first layer neuron
		for (int j=0; j<4; ++j)
		{
			double cmean = 0.0;
			for (int k=0; k<64; ++k)
				cmean += bdw[offt[j*64+k]];
			mean[j] = cmean/64.0;
		}
		// Factor mean removal and 1.0/127.5 scaling 
		// into first layer weights. scale to int16 range
		for (int j=0; j<4; ++j)
		{
			double mval = 0.0;
			for (int k=0; k<64; ++k)
				mval = max(mval,fabs((bdw[offt[j*64+k]]-mean[j])/127.5));
			const double scale = 32767.0/mval;
			for (int k=0; k<64; ++k)
				ws[offt[j*64+k]] = roundds(((bdw[offt[j*64+k]]-mean[j])/127.5)*scale);
			wf[j] = (float)(mval/32767.0);
		}
		memcpy(wf+4,bdw+4*64,(dims0new-4*64)*sizeof(float));
		free(offt);
	}
	else // using old prescreener
	{
		double mean[4] = { 0.0, 0.0, 0.0, 0.0 };
		// Calculate mean weight of each first layer neuron
		for (int j=0; j<4; ++j)
		{
			double cmean = 0.0;
			for (int k=0; k<48; ++k)
				cmean += bdata[j*48+k];
			mean[j] = cmean/48.0;
		}
		if (fapprox&1) // use int16 dot products in first layer
		{
			short *ws = (short*)weights0;
			float *wf = (float*)&ws[4*48];
			// Factor mean removal and 1.0/127.5 scaling 
			// into first layer weights. scale to int16 range
			for (int j=0; j<4; ++j)
			{
				double mval = 0.0;
				for (int k=0; k<48; ++k)
					mval = max(mval,fabs((bdata[j*48+k]-mean[j])/127.5));
				const double scale = 32767.0/mval;
				for (int k=0; k<48; ++k)
					ws[j*48+k] = roundds(((bdata[j*48+k]-mean[j])/127.5)*scale);
				wf[j] = (float)(mval/32767.0);
			}
			memcpy(wf+4,bdata+4*48,(dims0-4*48)*sizeof(float));
			if (opt > 1) // shuffle weight order for asm
			{
				short *rs = (short*)malloc(dims0*sizeof(float));
				memcpy(rs,weights0,dims0*sizeof(float));
				for (int j=0; j<4; ++j)
					for (int k=0; k<48; ++k)
						ws[(k>>3)*32+j*8+(k&7)] = rs[j*48+k];
				shufflePreScrnL2L3(wf+8,((float*)&rs[4*48])+8,opt);
				free(rs);
			}
		}
		else // use float dot products in first layer
		{
			// Factor mean removal and 1.0/127.5 scaling 
			// into first layer weights.
			for (int j=0; j<4; ++j)
				for (int k=0; k<48; ++k)
					weights0[j*48+k] = (bdata[j*48+k]-mean[j])/127.5;
			memcpy(weights0+4*48,bdata+4*48,(dims0-4*48)*sizeof(float));
			if (opt > 1) // shuffle weight order for asm
			{
				float *wf = weights0;
				float *rf = (float*)malloc(dims0*sizeof(float));
				memcpy(rf,weights0,dims0*sizeof(float));
				for (int j=0; j<4; ++j)
					for (int k=0; k<48; ++k)
						wf[(k>>2)*16+j*4+(k&3)] = rf[j*48+k];
				shufflePreScrnL2L3(wf+4*49,rf+4*49,opt);
				free(rf);
			}
		}
	}
	// Adjust prediction weights
	for (int i=0; i<2; ++i)
	{
		const float *bdataT = bdata+dims0+dims0new*3+dims1tsize*etype+dims1offset+i*dims1;
		const int nnst = nnsTable[nns];
		const int asize = xdiaTable[nsize]*ydiaTable[nsize];
		const int boff = nnst*2*asize;
		double *mean = (double*)calloc(asize+1+nnst*2,sizeof(double));
		// Calculate mean weight of each neuron (ignore bias)
		for (int j=0; j<nnst*2; ++j)
		{
			double cmean = 0.0;
			for (int k=0; k<asize; ++k)
				cmean += bdataT[j*asize+k];
			mean[asize+1+j] = cmean/(double)asize;
		}
		// Calculate mean softmax neuron
		for (int j=0; j<nnst; ++j)
		{
			for (int k=0; k<asize; ++k)
				mean[k] += bdataT[j*asize+k]-mean[asize+1+j];
			mean[asize] += bdataT[boff+j];
		}
		for (int j=0; j<asize+1; ++j)
			mean[j] /= (double)(nnst);
		if (fapprox&2) // use int16 dot products
		{
			short *ws = (short*)weights1[i];
			float *wf = (float*)&ws[nnst*2*asize];
			// Factor mean removal into weights, remove global offset from
			// softmax neurons, and scale weights to int16 range.
			for (int j=0; j<nnst; ++j) // softmax neurons
			{
				double mval = 0.0;
				for (int k=0; k<asize; ++k)
					mval = max(mval,fabs(bdataT[j*asize+k]-mean[asize+1+j]-mean[k]));
				const double scale = 32767.0/mval;
				for (int k=0; k<asize; ++k)
					ws[j*asize+k] = roundds((bdataT[j*asize+k]-mean[asize+1+j]-mean[k])*scale);
				wf[(j>>2)*8+(j&3)] = (float)(mval/32767.0);
				wf[(j>>2)*8+(j&3)+4] = bdataT[boff+j]-mean[asize];
			}
			for (int j=nnst; j<nnst*2; ++j) // elliott neurons
			{
				double mval = 0.0;
				for (int k=0; k<asize; ++k)
					mval = max(mval,fabs(bdataT[j*asize+k]-mean[asize+1+j]));
				const double scale = 32767.0/mval;
				for (int k=0; k<asize; ++k)
					ws[j*asize+k] = roundds((bdataT[j*asize+k]-mean[asize+1+j])*scale);
				wf[(j>>2)*8+(j&3)] = (float)(mval/32767.0);
				wf[(j>>2)*8+(j&3)+4] = bdataT[boff+j];
			}
			if (opt > 1) // shuffle weight order for asm
			{
				short *rs = (short*)malloc(nnst*2*asize*sizeof(short));
				memcpy(rs,ws,nnst*2*asize*sizeof(short));
				for (int j=0; j<nnst*2; ++j)
					for (int k=0; k<asize; ++k)
						ws[(j>>2)*asize*4+(k>>3)*32+(j&3)*8+(k&7)] = rs[j*asize+k];
				free(rs);
			}
		}
		else // use float dot products
		{
			// Factor mean removal into weights, and remove global
			// offset from softmax neurons.
			for (int j=0; j<nnst*2; ++j)
			{
				for (int k=0; k<asize; ++k)
				{
					const double q = j < nnst ? mean[k] : 0.0;
					if (opt > 1) // shuffle weight order for asm
						weights1[i][(j>>2)*asize*4+(k>>2)*16+(j&3)*4+(k&3)] = 
							bdataT[j*asize+k]-mean[asize+1+j]-q;
					else
						weights1[i][j*asize+k] = bdataT[j*asize+k]-mean[asize+1+j]-q;
				}
				weights1[i][boff+j] = bdataT[boff+j]-(j<nnst?mean[asize]:0.0);
			}
		}
		free(mean);
	}
	tids = (unsigned*)malloc(threads*sizeof(unsigned));
	thds = (HANDLE*)malloc(threads*sizeof(HANDLE));
	pssInfo = (PS_INFO**)malloc(threads*sizeof(PS_INFO*));
	int hslice[3], hremain[3];
	int srow[3] = { 6, 6, 6 };
	for (int i=0; i<3; ++i)
	{
		const int height = srcPF->GetHeight(i)-12;
		hslice[i] = height/threads;
		hremain[i] = height%threads;
	}
	for (int i=0; i<threads; ++i)
	{
		pssInfo[i] = (PS_INFO*)malloc(sizeof(PS_INFO));
		pssInfo[i]->type = 0;
		pssInfo[i]->input = (float*)_aligned_malloc(512*sizeof(float),16);
		pssInfo[i]->temp = (float*)_aligned_malloc(2048*sizeof(float),16);
		pssInfo[i]->weights0 = weights0;
		pssInfo[i]->weights1 = weights1;
		pssInfo[i]->ident = i;
		pssInfo[i]->qual = qual;
		pssInfo[i]->pscrn = pscrn;
		pssInfo[i]->env = env;
		pssInfo[i]->opt = opt;
		pssInfo[i]->Y = Y;
		pssInfo[i]->U = U;
		pssInfo[i]->V = V;
		pssInfo[i]->nns = nnsTable[nns];
		pssInfo[i]->xdia = xdiaTable[nsize];
		pssInfo[i]->ydia = ydiaTable[nsize];
		pssInfo[i]->asize = xdiaTable[nsize]*ydiaTable[nsize];
		pssInfo[i]->fapprox = fapprox;
		for (int b=0; b<3; ++b)
		{
			pssInfo[i]->lcount[b] = lcount[b];
			pssInfo[i]->dstp[b] = dstPF->GetPtr(b);
			pssInfo[i]->srcp[b] = srcPF->GetPtr(b);
			pssInfo[i]->dst_pitch[b] = dstPF->GetPitch(b);
			pssInfo[i]->src_pitch[b] = srcPF->GetPitch(b);
			pssInfo[i]->height[b] = srcPF->GetHeight(b);
			pssInfo[i]->width[b] = srcPF->GetWidth(b);
			pssInfo[i]->sheight[b] = srow[b];
			srow[b] += i == 0 ? hslice[b]+hremain[b] : hslice[b];
			pssInfo[i]->eheight[b] = srow[b];
		}
		pssInfo[i]->jobFinished = CreateEvent(NULL, TRUE, TRUE, NULL);
		pssInfo[i]->nextJob = CreateEvent(NULL, TRUE, FALSE, NULL);
		thds[i] = (HANDLE)_beginthreadex(0,0,&threadPool,(void*)(pssInfo[i]),0,&tids[i]);
	}
}

nnedi3::~nnedi3()
{
	delete srcPF;
	delete dstPF;
	_aligned_free(weights0);
	for (int i=0; i<2; ++i)
		_aligned_free(weights1[i]);
	for (int i=0; i<3; ++i)
		_aligned_free(lcount[i]);
	for (int i=0; i<threads; ++i)
	{
		pssInfo[i]->type = -1;
		SetEvent(pssInfo[i]->nextJob);
	}
	WaitForMultipleObjects(threads,thds,TRUE,INFINITE);
	for (int i=0; i<threads; ++i)
		CloseHandle(thds[i]);
	free(tids);
	free(thds);
	for (int i=0; i<threads; ++i)
	{
		CloseHandle(pssInfo[i]->jobFinished);
		CloseHandle(pssInfo[i]->nextJob);
		_aligned_free(pssInfo[i]->input);
		_aligned_free(pssInfo[i]->temp);
		free(pssInfo[i]);
	}
	free(pssInfo);
}

PVideoFrame __stdcall nnedi3::GetFrame(int n, IScriptEnvironment *env)
{
	int field_n;
	if (field > 1)
	{
		if (n&1) field_n = field == 3 ? 0 : 1;
		else field_n = field == 3 ? 1 : 0;
	}
	else field_n = field;
	copyPad(field>1?(n>>1):n,field_n,env);
	for (int i=0; i<3; ++i)
		memset(lcount[i],0,dstPF->GetHeight(i)*sizeof(int));
	PVideoFrame dst = env->NewVideoFrame(vi);
	const int plane[3] = { PLANAR_Y, PLANAR_U, PLANAR_V };
	for (int i=0; i<threads; ++i)
	{
		for (int b=0; b<3; ++b)
		{
			const int srow = pssInfo[i]->sheight[b];
			pssInfo[i]->field[b] = (srow&1) ? 1-field_n : field_n;
			if (vi.IsYV12())
			{
				pssInfo[i]->dstp[b] = dst->GetWritePtr(plane[b]);
				pssInfo[i]->dst_pitch[b] = dst->GetPitch(plane[b]);
			}
		}
		pssInfo[i]->type = 0;
		ResetEvent(pssInfo[i]->jobFinished);
		SetEvent(pssInfo[i]->nextJob);
	}
	for (int i=0; i<threads; ++i)
		WaitForSingleObject(pssInfo[i]->jobFinished,INFINITE);
	calcStartEnd2(vi.IsYV12()?dst:NULL);
	for (int i=0; i<threads; ++i)
	{
		pssInfo[i]->type = 1;
		ResetEvent(pssInfo[i]->jobFinished);
		SetEvent(pssInfo[i]->nextJob);
	}
	for (int i=0; i<threads; ++i)
		WaitForSingleObject(pssInfo[i]->jobFinished,INFINITE);
	if (!vi.IsYV12())
		dstPF->copyTo(dst, vi);
	return dst;
}

void nnedi3::copyPad(int n, int fn, IScriptEnvironment *env)
{
	const int off = 1-fn;
	PVideoFrame src = child->GetFrame(n, env);
	if (!dh)
	{
		if (vi.IsYV12())
		{
			const int plane[3] = { PLANAR_Y, PLANAR_U, PLANAR_V };
			for (int b=0; b<3; ++b)
				env->BitBlt(srcPF->GetPtr(b)+srcPF->GetPitch(b)*(6+off)+32,
					srcPF->GetPitch(b)*2,
					src->GetReadPtr(plane[b])+src->GetPitch(plane[b])*off,
					src->GetPitch(plane[b])*2,src->GetRowSize(plane[b]),
					src->GetHeight(plane[b])>>1);
		}
		else if (vi.IsYUY2())
		{
			srcPF->convYUY2to422(src->GetReadPtr()+src->GetPitch()*off,
				srcPF->GetPtr(0)+srcPF->GetPitch(0)*(6+off)+32,
				srcPF->GetPtr(1)+srcPF->GetPitch(1)*(6+off)+32,
				srcPF->GetPtr(2)+srcPF->GetPitch(2)*(6+off)+32,
				src->GetPitch()*2,srcPF->GetPitch(0)*2,srcPF->GetPitch(1)*2,
				vi.width,vi.height>>1);
		}
		else
		{
			srcPF->convRGB24to444(src->GetReadPtr()+(vi.height-1-off)*src->GetPitch(),
				srcPF->GetPtr(0)+srcPF->GetPitch(0)*(6+off)+32,
				srcPF->GetPtr(1)+srcPF->GetPitch(1)*(6+off)+32,
				srcPF->GetPtr(2)+srcPF->GetPitch(2)*(6+off)+32,
				-src->GetPitch()*2,srcPF->GetPitch(0)*2,srcPF->GetPitch(1)*2,
				vi.width,vi.height>>1);
		}
	}
	else
	{
		if (vi.IsYV12())
		{
			const int plane[3] = { PLANAR_Y, PLANAR_U, PLANAR_V };
			for (int b=0; b<3; ++b)
				env->BitBlt(srcPF->GetPtr(b)+srcPF->GetPitch(b)*(6+off)+32,
					srcPF->GetPitch(b)*2,src->GetReadPtr(plane[b]),
					src->GetPitch(plane[b]),src->GetRowSize(plane[b]),
					src->GetHeight(plane[b]));
		}
		else if (vi.IsYUY2())
		{
			srcPF->convYUY2to422(src->GetReadPtr(),
				srcPF->GetPtr(0)+srcPF->GetPitch(0)*(6+off)+32,
				srcPF->GetPtr(1)+srcPF->GetPitch(1)*(6+off)+32,
				srcPF->GetPtr(2)+srcPF->GetPitch(2)*(6+off)+32,
				src->GetPitch(),srcPF->GetPitch(0)*2,srcPF->GetPitch(1)*2,
				vi.width,vi.height>>1);
		}
		else
		{
			srcPF->convRGB24to444(src->GetReadPtr()+((vi.height>>1)-1)*src->GetPitch(),
				srcPF->GetPtr(0)+srcPF->GetPitch(0)*(6+off)+32,
				srcPF->GetPtr(1)+srcPF->GetPitch(1)*(6+off)+32,
				srcPF->GetPtr(2)+srcPF->GetPitch(2)*(6+off)+32,
				-src->GetPitch(),srcPF->GetPitch(0)*2,srcPF->GetPitch(1)*2,
				vi.width,vi.height>>1);
		}
	}
	for (int b=0; b<3; ++b)
	{
		unsigned char *dstp = srcPF->GetPtr(b);
		const int dst_pitch = srcPF->GetPitch(b);
		const int height = srcPF->GetHeight(b);
		const int width = srcPF->GetWidth(b);
		dstp += (6+off)*dst_pitch;
		for (int y=6+off; y<height-6; y+=2)
		{
			for (int x=0; x<32; ++x)
				dstp[x] = dstp[64-x];
			int c = 2;
			for (int x=width-32; x<width; ++x, c+=2)
				dstp[x] = dstp[x-c]; 
			dstp += dst_pitch*2;
		}
		dstp = srcPF->GetPtr(b);
		for (int y=off; y<6; y+=2)
			env->BitBlt(dstp+y*dst_pitch,dst_pitch,
				dstp+(12+2*off-y)*dst_pitch,dst_pitch,width,1);
		int c = 4;
		for (int y=height-6+off; y<height; y+=2, c+=4)
			env->BitBlt(dstp+y*dst_pitch,dst_pitch,
				dstp+(y-c)*dst_pitch,dst_pitch,width,1);
	}
}

void nnedi3::calcStartEnd2(PVideoFrame dst)
{
	const int plane[3] = { PLANAR_Y, PLANAR_U, PLANAR_V };
	for (int b=0; b<3; ++b)
	{
		if ((b == 0 && !Y) || (b == 1 && !U) || (b == 2 && !V))
			continue;
		const unsigned char *mskp = dst ? dst->GetReadPtr(plane[b]) : dstPF->GetPtr(b);
		const int pitch = dst ? dst->GetPitch(plane[b]) : dstPF->GetPitch(b);
		const int width = dstPF->GetWidth(b);
		const int height = dstPF->GetHeight(b);
		int total = 0, fl = -1, ll = 0;
		for (int j=0; j<height; ++j)
		{ 
			total += lcount[b][j];
			if (fl < 0 && lcount[b][j] > 0) fl = j;
		}
		if (total == 0)
			fl = height;
		else
		{
			for (int j=height-1; j>=0; --j)
			{
				if (lcount[b][j])
					break;
				++ll;
			}
		}
		int tslice = int(total/double(threads)+0.95);
		int count=0, countt=0, y=fl, yl=fl, th=0;
		while (y < height-ll)
		{
			count += lcount[b][y++];
			if (count >= tslice)
			{
				pssInfo[th]->sheight2[b] = yl;
				countt += count;
				if (countt == total)
					y = height-ll;
				pssInfo[th]->eheight2[b] = y;
				while (y < height-ll && lcount[b][y] == 0)
					++y;
				yl = y;
				count = 0;
				++th;
			}
		}
		if (yl != y)
		{
			pssInfo[th]->sheight2[b] = yl;
			countt += count;
			if (countt == total)
				y = height-ll;
			pssInfo[th]->eheight2[b] = y;
			++th;
		}
		for (; th<threads; ++th)
			pssInfo[th]->sheight2[b] = pssInfo[th]->eheight2[b] = height;
	}
}

__declspec(align(16)) const __int64 sign_bits_f[2] = { 0x7FFFFFFF7FFFFFFF, 0x7FFFFFFF7FFFFFFF };
__declspec(align(16)) const __int64 sign_bits_f_zero_l[2] = { 0x7FFFFFFF00000000, 0x7FFFFFFF7FFFFFFF };
__declspec(align(16)) const float ones_f[4] = { 1.0f, 1.0f, 1.0f, 1.0f };

void elliott_C(float *data, const int n)
{
	for (int i=0; i<n; ++i)
		data[i] = data[i]/(1.0f+fabsf(data[i]));
}

void dotProd_C(const float *data, const float *weights, 
	float *vals, const int n, const int len, const float *scale)
{
	for (int i=0; i<n; ++i)
	{
		float sum = 0.0f;
		for (int j=0; j<len; ++j)
			sum += data[j]*weights[i*len+j];
		vals[i] = sum*scale[0]+weights[n*len+i];
	}
}

void dotProdS_C(const float *dataf, const float *weightsf, 
	float *vals, const int n, const int len, const float *scale)
{
	const short *data = (short*)dataf;
	const short *weights = (short*)weightsf;
	const float *wf = (float*)&weights[n*len];
	for (int i=0; i<n; ++i)
	{
		int sum = 0, off = ((i>>2)<<3)+(i&3);
		for (int j=0; j<len; ++j)
			sum += data[j]*weights[i*len+j];
		vals[i] = sum*wf[off]*scale[0]+wf[off+4];
	}
}

void computeNetwork0_C(const float *input, const float *weights, unsigned char *d)
{
	float temp[12], scale = 1.0f;
	dotProd_C(input,weights,temp,4,48,&scale);
	const float t = temp[0];
	elliott_C(temp,4);
	temp[0] = t;
	dotProd_C(temp,weights+4*49,temp+4,4,4,&scale);
	elliott_C(temp+4,4);
	dotProd_C(temp,weights+4*49+4*5,temp+8,4,8,&scale);
	if (max(temp[10],temp[11]) <= max(temp[8],temp[9]))
		d[0] = 1;
	else
		d[0] = 0;
}

__declspec(naked) void computeNetwork0_SSE2(const float *input, const float *weights,
	unsigned char *d)
{
	__asm
	{
		// 	dotProd48_m4_SSE(input,weights,temp,4);
		mov ecx,[esp+4]
		mov edx,[esp+8]
		mov eax,1
		movaps xmm0,[ecx]
		movaps xmm1,xmm0
		movaps xmm2,xmm0
		movaps xmm3,xmm0
		mulps xmm0,[edx]
		mulps xmm1,[edx+16]
		mulps xmm2,[edx+32]
		mulps xmm3,[edx+48]
		movaps xmm4,[ecx+16]
		movaps xmm5,xmm4
		movaps xmm6,xmm4
		movaps xmm7,xmm4
		mulps xmm4,[edx+64]
		mulps xmm5,[edx+80]
		mulps xmm6,[edx+96]
		mulps xmm7,[edx+112]
		addps xmm0,xmm4
		addps xmm1,xmm5
		addps xmm2,xmm6
		addps xmm3,xmm7
		movaps xmm4,[ecx+32]
		movaps xmm5,xmm4
		movaps xmm6,xmm4
		movaps xmm7,xmm4
		mulps xmm4,[edx+128]
		mulps xmm5,[edx+144]
		mulps xmm6,[edx+160]
		mulps xmm7,[edx+176]
		addps xmm0,xmm4
		addps xmm1,xmm5
		addps xmm2,xmm6
		addps xmm3,xmm7
		movaps xmm4,[ecx+48]
		movaps xmm5,xmm4
		movaps xmm6,xmm4
		movaps xmm7,xmm4
		mulps xmm4,[edx+192]
		mulps xmm5,[edx+208]
		mulps xmm6,[edx+224]
		mulps xmm7,[edx+240]
		addps xmm0,xmm4
		addps xmm1,xmm5
		addps xmm2,xmm6
		addps xmm3,xmm7
		movaps xmm4,[ecx+64]
		movaps xmm5,xmm4
		movaps xmm6,xmm4
		movaps xmm7,xmm4
		mulps xmm4,[edx+256]
		mulps xmm5,[edx+272]
		mulps xmm6,[edx+288]
		mulps xmm7,[edx+304]
		addps xmm0,xmm4
		addps xmm1,xmm5
		addps xmm2,xmm6
		addps xmm3,xmm7
		movaps xmm4,[ecx+80]
		movaps xmm5,xmm4
		movaps xmm6,xmm4
		movaps xmm7,xmm4
		mulps xmm4,[edx+320]
		mulps xmm5,[edx+336]
		mulps xmm6,[edx+352]
		mulps xmm7,[edx+368]
		addps xmm0,xmm4
		addps xmm1,xmm5
		addps xmm2,xmm6
		addps xmm3,xmm7
		movaps xmm4,[ecx+96]
		movaps xmm5,xmm4
		movaps xmm6,xmm4
		movaps xmm7,xmm4
		mulps xmm4,[edx+384]
		mulps xmm5,[edx+400]
		mulps xmm6,[edx+416]
		mulps xmm7,[edx+432]
		addps xmm0,xmm4
		addps xmm1,xmm5
		addps xmm2,xmm6
		addps xmm3,xmm7
		movaps xmm4,[ecx+112]
		movaps xmm5,xmm4
		movaps xmm6,xmm4
		movaps xmm7,xmm4
		mulps xmm4,[edx+448]
		mulps xmm5,[edx+464]
		mulps xmm6,[edx+480]
		mulps xmm7,[edx+496]
		addps xmm0,xmm4
		addps xmm1,xmm5
		addps xmm2,xmm6
		addps xmm3,xmm7
		movaps xmm4,[ecx+128]
		movaps xmm5,xmm4
		movaps xmm6,xmm4
		movaps xmm7,xmm4
		mulps xmm4,[edx+512]
		mulps xmm5,[edx+528]
		mulps xmm6,[edx+544]
		mulps xmm7,[edx+560]
		addps xmm0,xmm4
		addps xmm1,xmm5
		addps xmm2,xmm6
		addps xmm3,xmm7
		movaps xmm4,[ecx+144]
		movaps xmm5,xmm4
		movaps xmm6,xmm4
		movaps xmm7,xmm4
		mulps xmm4,[edx+576]
		mulps xmm5,[edx+592]
		mulps xmm6,[edx+608]
		mulps xmm7,[edx+624]
		addps xmm0,xmm4
		addps xmm1,xmm5
		addps xmm2,xmm6
		addps xmm3,xmm7
		movaps xmm4,[ecx+160]
		movaps xmm5,xmm4
		movaps xmm6,xmm4
		movaps xmm7,xmm4
		mulps xmm4,[edx+640]
		mulps xmm5,[edx+656]
		mulps xmm6,[edx+672]
		mulps xmm7,[edx+688]
		addps xmm0,xmm4
		addps xmm1,xmm5
		addps xmm2,xmm6
		addps xmm3,xmm7
		movaps xmm4,[ecx+176]
		movaps xmm5,xmm4
		movaps xmm6,xmm4
		movaps xmm7,xmm4
		mulps xmm4,[edx+704]
		mulps xmm5,[edx+720]
		mulps xmm6,[edx+736]
		mulps xmm7,[edx+752]
		addps xmm0,xmm4
		addps xmm1,xmm5
		addps xmm2,xmm6
		addps xmm3,xmm7
		movaps xmm4,xmm0
		movaps xmm5,xmm2
		unpcklpd xmm0,xmm1
		unpcklpd xmm2,xmm3
		unpckhpd xmm4,xmm1
		unpckhpd xmm5,xmm3
		addps xmm0,xmm4
		addps xmm2,xmm5
		movaps xmm6,xmm0
		shufps xmm0,xmm2,136
		shufps xmm6,xmm2,221
		addps xmm0,xmm6
		addps xmm0,[edx+768]
		// const float t = temp[0];
		// elliott4_SSE(temp);
		// temp[0] = t;
		movaps xmm1,xmm0
		andps xmm0,sign_bits_f_zero_l
		addps xmm0,ones_f
		rcpps xmm0,xmm0
		mulps xmm0,xmm1
		// 	dotProd4_m4_SSE2(temp,weights+4*49,temp+4,4);
		pshufd xmm1,xmm0,0
		pshufd xmm2,xmm0,85
		pshufd xmm3,xmm0,170
		pshufd xmm4,xmm0,255
		mulps xmm1,[edx+784]
		mulps xmm2,[edx+784+16]
		mulps xmm3,[edx+784+32]
		mulps xmm4,[edx+784+48]
		addps xmm1,xmm2
		addps xmm3,xmm4
		addps xmm1,xmm3
		addps xmm1,[edx+784+64]
		// elliott4_SSE(temp+4);
		movaps xmm7,xmm1
		andps xmm1,sign_bits_f
		movaps xmm3,xmm0
		addps xmm1,ones_f
		rcpps xmm1,xmm1
		mulps xmm7,xmm1
		// 	dotProd8_m4_SSE2(temp,weights+4*49+4*5,temp+32,4);
		pshufd xmm0,xmm0,0
		pshufd xmm1,xmm3,85
		pshufd xmm2,xmm3,170
		pshufd xmm3,xmm3,255
		mulps xmm0,[edx+864]
		mulps xmm1,[edx+864+16]
		mulps xmm2,[edx+864+32]
		mulps xmm3,[edx+864+48]
		pshufd xmm4,xmm7,0
		pshufd xmm5,xmm7,85
		pshufd xmm6,xmm7,170
		pshufd xmm7,xmm7,255
		mulps xmm4,[edx+864+64]
		mulps xmm5,[edx+864+80]
		mulps xmm6,[edx+864+96]
		mulps xmm7,[edx+864+112]
		addps xmm0,xmm1
		addps xmm2,xmm3
		addps xmm4,xmm5
		addps xmm6,xmm7
		addps xmm0,xmm2
		addps xmm4,xmm6
		addps xmm0,xmm4
		mov ecx,[esp+12]
		addps xmm0,[edx+864+128]
		movhlps xmm1,xmm0
		maxps xmm0,xmm1
		pshuflw xmm1,xmm0,14
		comiss xmm1,xmm0
		jbe finish
		xor eax,eax
finish:
		mov BYTE PTR[ecx],al
		ret
	}
}

void computeNetwork0_i16_C(const float *inputf, const float *weightsf, unsigned char *d)
{
	const float *wf = weightsf+2*48;
	float temp[12], scale = 1.0f;
	dotProdS_C(inputf,weightsf,temp,4,48,&scale);
	const float t = temp[0];
	elliott_C(temp,4);
	temp[0] = t;
	dotProd_C(temp,wf+8,temp+4,4,4,&scale);
	elliott_C(temp+4,4);
	dotProd_C(temp,wf+8+4*5,temp+8,4,8,&scale);
	if (max(temp[10],temp[11]) <= max(temp[8],temp[9]))
		d[0] = 1;
	else
		d[0] = 0;
}

__declspec(naked) void computeNetwork0_i16_SSE2(const float *inputf, const float *weightsf,
	unsigned char *d)
{
	__asm
	{
		mov ecx,[esp+4]
		mov edx,[esp+8]
		mov eax,1
		movdqa xmm0,[ecx]
		movdqa xmm1,xmm0
		movdqa xmm2,xmm0
		movdqa xmm3,xmm0
		pmaddwd xmm0,[edx]
		pmaddwd xmm1,[edx+16]
		pmaddwd xmm2,[edx+32]
		pmaddwd xmm3,[edx+48]
		movdqa xmm4,[ecx+16]
		movdqa xmm5,xmm4
		movdqa xmm6,xmm4
		movdqa xmm7,xmm4
		pmaddwd xmm4,[edx+64]
		pmaddwd xmm5,[edx+80]
		pmaddwd xmm6,[edx+96]
		pmaddwd xmm7,[edx+112]
		paddd xmm0,xmm4
		paddd xmm1,xmm5
		paddd xmm2,xmm6
		paddd xmm3,xmm7
		movdqa xmm4,[ecx+32]
		movdqa xmm5,xmm4
		movdqa xmm6,xmm4
		movdqa xmm7,xmm4
		pmaddwd xmm4,[edx+128]
		pmaddwd xmm5,[edx+144]
		pmaddwd xmm6,[edx+160]
		pmaddwd xmm7,[edx+176]
		paddd xmm0,xmm4
		paddd xmm1,xmm5
		paddd xmm2,xmm6
		paddd xmm3,xmm7
		movdqa xmm4,[ecx+48]
		movdqa xmm5,xmm4
		movdqa xmm6,xmm4
		movdqa xmm7,xmm4
		pmaddwd xmm4,[edx+192]
		pmaddwd xmm5,[edx+208]
		pmaddwd xmm6,[edx+224]
		pmaddwd xmm7,[edx+240]
		paddd xmm0,xmm4
		paddd xmm1,xmm5
		paddd xmm2,xmm6
		paddd xmm3,xmm7
		movdqa xmm4,[ecx+64]
		movdqa xmm5,xmm4
		movdqa xmm6,xmm4
		movdqa xmm7,xmm4
		pmaddwd xmm4,[edx+256]
		pmaddwd xmm5,[edx+272]
		pmaddwd xmm6,[edx+288]
		pmaddwd xmm7,[edx+304]
		paddd xmm0,xmm4
		paddd xmm1,xmm5
		paddd xmm2,xmm6
		paddd xmm3,xmm7
		movdqa xmm4,[ecx+80]
		movdqa xmm5,xmm4
		movdqa xmm6,xmm4
		movdqa xmm7,xmm4
		pmaddwd xmm4,[edx+320]
		pmaddwd xmm5,[edx+336]
		pmaddwd xmm6,[edx+352]
		pmaddwd xmm7,[edx+368]
		paddd xmm0,xmm4
		paddd xmm1,xmm5
		paddd xmm2,xmm6
		paddd xmm3,xmm7
		movdqa xmm4,xmm0
		movdqa xmm5,xmm2
		punpcklqdq xmm0,xmm1
		punpcklqdq xmm2,xmm3
		punpckhqdq xmm4,xmm1
		punpckhqdq xmm5,xmm3
		paddd xmm0,xmm4
		paddd xmm2,xmm5
		movdqa xmm6,xmm0
		shufps xmm0,xmm2,136
		shufps xmm6,xmm2,221
		paddd xmm0,xmm6
		cvtdq2ps xmm0,xmm0
		mulps xmm0,[edx+384]
		addps xmm0,[edx+400]
		// const float t = temp[0];
		// elliott4_SSE(temp);
		// temp[0] = t;
		movaps xmm1,xmm0
		andps xmm0,sign_bits_f_zero_l
		addps xmm0,ones_f
		rcpps xmm0,xmm0
		mulps xmm0,xmm1
		// 	dotProd4_m4_SSE2(temp,weights+4*49,temp+4,4);
		pshufd xmm1,xmm0,0
		pshufd xmm2,xmm0,85
		pshufd xmm3,xmm0,170
		pshufd xmm4,xmm0,255
		mulps xmm1,[edx+416]
		mulps xmm2,[edx+416+16]
		mulps xmm3,[edx+416+32]
		mulps xmm4,[edx+416+48]
		addps xmm1,xmm2
		addps xmm3,xmm4
		addps xmm1,xmm3
		addps xmm1,[edx+416+64]
		// elliott4_SSE(temp+4);
		movaps xmm7,xmm1
		andps xmm1,sign_bits_f
		movaps xmm3,xmm0
		addps xmm1,ones_f
		rcpps xmm1,xmm1
		mulps xmm7,xmm1
		// 	dotProd8_m4_SSE2(temp,weights+4*49+4*5,temp+32,4);
		pshufd xmm0,xmm0,0
		pshufd xmm1,xmm3,85
		pshufd xmm2,xmm3,170
		pshufd xmm3,xmm3,255
		mulps xmm0,[edx+496]
		mulps xmm1,[edx+496+16]
		mulps xmm2,[edx+496+32]
		mulps xmm3,[edx+496+48]
		pshufd xmm4,xmm7,0
		pshufd xmm5,xmm7,85
		pshufd xmm6,xmm7,170
		pshufd xmm7,xmm7,255
		mulps xmm4,[edx+496+64]
		mulps xmm5,[edx+496+80]
		mulps xmm6,[edx+496+96]
		mulps xmm7,[edx+496+112]
		addps xmm0,xmm1
		addps xmm2,xmm3
		addps xmm4,xmm5
		addps xmm6,xmm7
		addps xmm0,xmm2
		addps xmm4,xmm6
		addps xmm0,xmm4
		mov ecx,[esp+12]
		addps xmm0,[edx+496+128]
		movhlps xmm1,xmm0
		maxps xmm0,xmm1
		pshuflw xmm1,xmm0,14
		comiss xmm1,xmm0
		jbe finish
		xor eax,eax
finish:
		mov BYTE PTR[ecx],al
		ret
	}
}

void uc2f48_C(const unsigned char *t, const int pitch, float *p)
{
	for (int y=0; y<4; ++y)
		for (int x=0; x<12; ++x)
			p[y*12+x] = t[y*pitch*2+x];
}

__declspec(naked) void uc2f48_SSE2(const unsigned char *t, const int pitch, float *p)
{
	__asm
	{
		mov eax,[esp+4]
		mov ecx,[esp+8]
		mov edx,[esp+12]
		pxor xmm6,xmm6
		movq xmm0,QWORD PTR[eax]
		movd xmm4,[eax+8]
		movq xmm2,QWORD PTR[eax+ecx*2]
		movd xmm5,[eax+ecx*2+8]
		punpcklbw xmm0,xmm6
		punpcklbw xmm4,xmm6
		punpcklbw xmm2,xmm6
		punpcklbw xmm5,xmm6
		movdqa xmm1,xmm0
		punpcklbw xmm4,xmm6
		movdqa xmm3,xmm2
		punpcklbw xmm5,xmm6
		punpcklbw xmm0,xmm6
		punpckhbw xmm1,xmm6
		punpcklbw xmm2,xmm6
		punpckhbw xmm3,xmm6
		lea eax,[eax+ecx*4]
		cvtdq2ps xmm4,xmm4
		cvtdq2ps xmm5,xmm5
		cvtdq2ps xmm0,xmm0
		cvtdq2ps xmm1,xmm1
		cvtdq2ps xmm2,xmm2
		cvtdq2ps xmm3,xmm3
		movaps [edx],xmm0
		movaps [edx+16],xmm1
		movaps [edx+32],xmm4
		movaps [edx+48],xmm2
		movaps [edx+64],xmm3
		movaps [edx+80],xmm5
		movq xmm0,QWORD PTR[eax]
		movd xmm4,[eax+8]
		movq xmm2,QWORD PTR[eax+ecx*2]
		movd xmm5,[eax+ecx*2+8]
		punpcklbw xmm0,xmm6
		punpcklbw xmm4,xmm6
		punpcklbw xmm2,xmm6
		punpcklbw xmm5,xmm6
		movdqa xmm1,xmm0
		punpcklbw xmm4,xmm6
		movdqa xmm3,xmm2
		punpcklbw xmm5,xmm6
		punpcklbw xmm0,xmm6
		punpckhbw xmm1,xmm6
		punpcklbw xmm2,xmm6
		punpckhbw xmm3,xmm6
		cvtdq2ps xmm4,xmm4
		cvtdq2ps xmm5,xmm5
		cvtdq2ps xmm0,xmm0
		cvtdq2ps xmm1,xmm1
		cvtdq2ps xmm2,xmm2
		cvtdq2ps xmm3,xmm3
		movaps [edx+96],xmm0
		movaps [edx+112],xmm1
		movaps [edx+128],xmm4
		movaps [edx+144],xmm2
		movaps [edx+160],xmm3
		movaps [edx+176],xmm5
		ret
	}
}

void uc2s48_C(const unsigned char *t, const int pitch, float *pf)
{
	short *p = (short*)pf;
	for (int y=0; y<4; ++y)
		for (int x=0; x<12; ++x)
			p[y*12+x] = t[y*pitch*2+x];
}

__declspec(naked) void uc2s48_SSE2(const unsigned char *t, const int pitch, float *pf)
{
	__asm
	{
		mov eax,[esp+4]
		mov ecx,[esp+8]
		lea edx,[eax+ecx*4]
		movq xmm0,QWORD PTR[eax]
		movd xmm1,[eax+8]
		movd xmm2,[eax+ecx*2]
		movq xmm3,QWORD PTR[eax+ecx*2+4]
		movq xmm4,QWORD PTR[edx]
		movd xmm5,[edx+8]
		movd xmm6,[edx+ecx*2]
		movq xmm7,QWORD PTR[edx+ecx*2+4]
		punpckldq xmm1,xmm2
		pxor xmm2,xmm2
		punpckldq xmm5,xmm6
		mov edx,[esp+12]
		punpcklbw xmm0,xmm2
		punpcklbw xmm3,xmm2
		punpcklbw xmm1,xmm2
		punpcklbw xmm4,xmm2
		punpcklbw xmm5,xmm2
		punpcklbw xmm7,xmm2
		movdqa [edx],xmm0
		movdqa [edx+16],xmm1
		movdqa [edx+32],xmm3
		movdqa [edx+48],xmm4
		movdqa [edx+64],xmm5
		movdqa [edx+80],xmm7
		ret
	}
}

int processLine0_C(const unsigned char *tempu, int width, unsigned char *dstp,
	const unsigned char *src3p, const int src_pitch)
{
	int count = 0;
	for (int x=0; x<width; ++x)
	{
		if (tempu[x])
			dstp[x] = CB2((19*(src3p[x+src_pitch*2]+src3p[x+src_pitch*4])-
				3*(src3p[x]+src3p[x+src_pitch*6])+16)>>5);
		else
		{
			dstp[x] = 255;
			++count;
		}
	}
	return count;
}

__declspec(align(16)) const unsigned char ub_1[16] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };
__declspec(align(16)) const short w_19[8] = { 19, 19, 19, 19, 19, 19, 19, 19 };
__declspec(align(16)) const short w_3[8] = { 3, 3, 3, 3, 3, 3, 3, 3 };
__declspec(align(16)) const short w_254[8] = { 254, 254, 254, 254, 254, 254, 254, 254 };
__declspec(align(16)) const unsigned short uw_16[8] = { 16, 16, 16, 16, 16, 16, 16, 16 };

int processLine0_SSE2(const unsigned char *tempu, int width, unsigned char *dstp,
	const unsigned char *src3p, const int src_pitch)
{
	int count;
	const int remain = width&15;
	width -= remain;
	if (!width)
		goto skipasm;
	__asm
	{
		mov eax,tempu
		mov ebx,src3p
		mov ecx,width
		mov edx,src_pitch
		mov esi,dstp
		lea edi,[ebx+edx*4]
		pxor xmm6,xmm6
		pxor xmm7,xmm7
xloop:
		movdqa xmm0,[ebx+edx*2]
		movdqa xmm1,[edi]
		movdqa xmm2,xmm0
		movdqa xmm3,xmm1
		punpcklbw xmm0,xmm7
		punpckhbw xmm2,xmm7
		punpcklbw xmm1,xmm7
		punpckhbw xmm3,xmm7
		paddw xmm0,xmm1
		paddw xmm2,xmm3
		pmullw xmm0,w_19
		pmullw xmm2,w_19
		movdqa xmm1,[ebx]
		movdqa xmm3,[edi+edx*2]
		movdqa xmm4,xmm1
		movdqa xmm5,xmm3
		punpcklbw xmm1,xmm7
		punpckhbw xmm4,xmm7
		punpcklbw xmm3,xmm7
		punpckhbw xmm5,xmm7
		paddw xmm1,xmm3
		paddw xmm4,xmm5
		pmullw xmm1,w_3
		pmullw xmm4,w_3
		movdqa xmm3,[eax]
		psubusw xmm0,xmm1
		psubusw xmm2,xmm4
		pcmpeqb xmm3,ub_1
		paddusw xmm0,uw_16
		paddusw xmm2,uw_16
		movdqa xmm1,xmm3
		pcmpeqb xmm4,xmm4
		psrlw xmm0,5
		psrlw xmm2,5
		pxor xmm1,xmm4
		pminsw xmm0,w_254
		pminsw xmm2,w_254
		movdqa xmm5,xmm1
		packuswb xmm0,xmm2
		pand xmm5,ub_1
		pand xmm0,xmm3
		psadbw xmm5,xmm7
		por xmm0,xmm1
		movdqa xmm2,xmm5
		psrldq xmm5,8
		movdqa [esi],xmm0
		paddusw xmm5,xmm2
		paddusw xmm6,xmm5
		add ebx,16
		add edi,16
		add eax,16
		add esi,16
		sub ecx,16
		jnz xloop
		movd count,xmm6
	}
skipasm:
	for (int x=width; x<width+remain; ++x)
	{
		if (tempu[x])
			dstp[x] = CB2((19*(src3p[x+src_pitch*2]+src3p[x+src_pitch*4])-
				3*(src3p[x]+src3p[x+src_pitch*6])+16)>>5);
		else
		{
			dstp[x] = 255;
			++count;
		}
	}
	return count;
}

void evalFunc_0(void *ps)
{
	PS_INFO *pss = (PS_INFO*)ps;
	float *input = pss->input;
	const float *weights0 = pss->weights0;
	float *temp = pss->temp;
	unsigned char *tempu = (unsigned char*)temp;
	const int opt = pss->opt;
	const int pscrn = pss->pscrn;
	const int fapprox = pss->fapprox;
	void (*uc2s)(const unsigned char*, const int, float*);
	void (*computeNetwork0)(const float*, const float*, unsigned char *d);
	int (*processLine0)(const unsigned char*, int, unsigned char*, const unsigned char*, const int);
	if (opt == 1) processLine0 = processLine0_C;
	else processLine0 = processLine0_SSE2;
	if (pscrn < 2) // original prescreener
	{
		if (fapprox&1) // int16 dot products
		{
			if (opt == 1) uc2s = uc2s48_C;
			else uc2s = uc2s48_SSE2;
			if (opt == 1) computeNetwork0 = computeNetwork0_i16_C;
			else computeNetwork0 = computeNetwork0_i16_SSE2;
		}
		else
		{
			if (opt == 1) uc2s = uc2f48_C;
			else uc2s = uc2f48_SSE2;
			if (opt == 1) computeNetwork0 = computeNetwork0_C;
			else computeNetwork0 = computeNetwork0_SSE2;
		}
	}
	else // new prescreener
	{
		// only int16 dot products
		if (opt == 1) uc2s = uc2s64_C;
		else uc2s = uc2s64_SSE2;
		if (opt == 1) computeNetwork0 = computeNetwork0new_C;
		else computeNetwork0 = computeNetwork0new_SSE2;
	}
	for (int b=0; b<3; ++b)
	{
		if ((b == 0 && !pss->Y) || 
			(b == 1 && !pss->U) ||
			(b == 2 && !pss->V))
			continue;
		const unsigned char *srcp = pss->srcp[b];
		const int src_pitch = pss->src_pitch[b];
		const int width = pss->width[b];
		unsigned char *dstp = pss->dstp[b];
		const int dst_pitch = pss->dst_pitch[b];
		pss->env->BitBlt(dstp+(pss->sheight[b]-5-pss->field[b])*dst_pitch,
			dst_pitch*2,srcp+(pss->sheight[b]+1-pss->field[b])*src_pitch+32,
			src_pitch*2,width-64,(pss->eheight[b]-pss->sheight[b]+pss->field[b])>>1);
		const int ystart = pss->sheight[b]+pss->field[b];
		const int ystop = pss->eheight[b];
		srcp += ystart*src_pitch;
		dstp += (ystart-6)*dst_pitch-32;
		const unsigned char *src3p = srcp-src_pitch*3;
		int *lcount = pss->lcount[b]-6;
		if (pss->pscrn == 1) // original
		{
			for (int y=ystart; y<ystop; y+=2)
			{
				for (int x=32; x<width-32; ++x)
				{
					uc2s(src3p+x-5,src_pitch,input);
					computeNetwork0(input,weights0,tempu+x);
				}
				lcount[y] += processLine0(tempu+32,width-64,dstp+32,src3p+32,src_pitch);
				src3p += src_pitch*2;
				dstp += dst_pitch*2;
			}
		}
		else if (pss->pscrn >= 2) // new
		{
			for (int y=ystart; y<ystop; y+=2)
			{
				for (int x=32; x<width-32; x+=4)
				{
					uc2s(src3p+x-6,src_pitch,input);
					computeNetwork0(input,weights0,tempu+x);
				}
				lcount[y] += processLine0(tempu+32,width-64,dstp+32,src3p+32,src_pitch);
				src3p += src_pitch*2;
				dstp += dst_pitch*2;
			}
		}
		else // no prescreening
		{
			for (int y=ystart; y<ystop; y+=2)
			{
				memset(dstp+32,255,width-64);
				lcount[y] += width-64;
				dstp += dst_pitch*2;
			}
		}
	}
}

void extract_m8_C(const unsigned char *srcp, const int stride, 
	const int xdia, const int ydia, float *mstd, float *input)
{
	int sum = 0, sumsq = 0;
	for (int y=0; y<ydia; ++y)
	{
		const unsigned char *srcpT = srcp+y*stride*2;
		for (int x=0; x<xdia; ++x, ++input)
		{
			sum += srcpT[x];
			sumsq += srcpT[x]*srcpT[x];
			input[0] = srcpT[x];
		}
	}
	const float scale = 1.0f/(float)(xdia*ydia);
	mstd[0] = sum*scale;
	mstd[1] = sumsq*scale-mstd[0]*mstd[0];
	mstd[3] = 0.0f;
	if (mstd[1] <= FLT_EPSILON)
		mstd[1] = mstd[2] = 0.0f;
	else
	{
		mstd[1] = sqrtf(mstd[1]);
		mstd[2] = 1.0f/mstd[1];
	}
}

__declspec(align(16)) const float flt_epsilon_sse[4] = { FLT_EPSILON, FLT_EPSILON, FLT_EPSILON, FLT_EPSILON };

__declspec(naked) void extract_m8_SSE2(const unsigned char *srcp, 
	const int stride, const int xdia, const int ydia, float *mstd, float *input)
{
	__asm
	{
		push ebp
		push ebx
		push edi
		push esi
		mov eax,[esp+20]
		mov ebx,[esp+24]
		mov edi,[esp+28]
		mov ebp,[esp+32]
		mov esi,[esp+40]
		lea edx,[eax+ebx*2]
		pxor xmm5,xmm5 // sum
		pxor xmm6,xmm6 // sumsq
		pxor xmm3,xmm3
yloop2:
		xor ecx,ecx
xloop2:
		movq xmm0,QWORD PTR[eax+ecx]
		movq xmm2,QWORD PTR[edx+ecx]
		punpcklbw xmm0,xmm3
		punpcklbw xmm2,xmm3
		movdqa xmm1,xmm0
		movdqa xmm4,xmm2
		punpcklwd xmm0,xmm3
		punpckhwd xmm1,xmm3
		punpcklwd xmm2,xmm3
		punpckhwd xmm4,xmm3
		cvtdq2ps xmm0,xmm0
		cvtdq2ps xmm1,xmm1
		cvtdq2ps xmm2,xmm2
		cvtdq2ps xmm4,xmm4
		movaps [esi],xmm0
		movaps [esi+16],xmm1
		movaps [esi+edi*4],xmm2
		movaps [esi+edi*4+16],xmm4
		addps xmm5,xmm0
		addps xmm5,xmm1
		addps xmm5,xmm2
		addps xmm5,xmm4
		mulps xmm0,xmm0
		mulps xmm1,xmm1
		mulps xmm2,xmm2
		mulps xmm4,xmm4
		addps xmm0,xmm1
		addps xmm2,xmm4
		addps xmm6,xmm0
		addps xmm6,xmm2
		add ecx,8
		add esi,32
		cmp ecx,edi
		jl xloop2
		lea eax,[eax+ebx*4]
		lea edx,[edx+ebx*4]
		lea esi,[esi+edi*4]
		sub ebp,2
		jnz yloop2
		mov eax,[esp+32]
		movhlps xmm0,xmm5
		movhlps xmm1,xmm6
		mul edi
		addps xmm5,xmm0
		addps xmm6,xmm1
		cvtsi2ss xmm7,eax
		pshuflw xmm0,xmm5,14
		pshuflw xmm1,xmm6,14
		rcpss xmm7,xmm7 // scale
		addss xmm5,xmm0
		addss xmm6,xmm1
		mov eax,[esp+36]
		mulss xmm5,xmm7 // mean
		mulss xmm6,xmm7
		movss [eax],xmm5
		mulss xmm5,xmm5
		subss xmm6,xmm5 // var
		comiss xmm6,flt_epsilon_sse
		jbe novarjmp
		rsqrtss xmm6,xmm6 // 1.0/std
		rcpss xmm5,xmm6 // std
		movss [eax+4],xmm5
		movss [eax+8],xmm6
		jmp finish
novarjmp:
		movss [eax+4],xmm3
		movss [eax+8],xmm3
finish:
		movss [eax+12],xmm3
		pop esi
		pop edi
		pop ebx
		pop ebp
		ret
	}
}

void extract_m8_i16_C(const unsigned char *srcp, const int stride, 
	const int xdia, const int ydia, float *mstd, float *inputf)
{
	short *input = (short*)inputf;
	int sum = 0, sumsq = 0;
	for (int y=0; y<ydia; ++y)
	{
		const unsigned char *srcpT = srcp+y*stride*2;
		for (int x=0; x<xdia; ++x, ++input)
		{
			sum += srcpT[x];
			sumsq += srcpT[x]*srcpT[x];
			input[0] = srcpT[x];
		}
	}
	const float scale = 1.0f/(float)(xdia*ydia);
	mstd[0] = sum*scale;
	mstd[1] = sumsq*scale-mstd[0]*mstd[0];
	mstd[3] = 0.0f;
	if (mstd[1] <= FLT_EPSILON)
		mstd[1] = mstd[2] = 0.0f;
	else
	{
		mstd[1] = sqrtf(mstd[1]);
		mstd[2] = 1.0f/mstd[1];
	}
}

__declspec(naked) void extract_m8_i16_SSE2(const unsigned char *srcp, 
	const int stride, const int xdia, const int ydia, float *mstd, float *inputf)
{
	__asm
	{
		push ebp
		push ebx
		push edi
		push esi
		mov eax,[esp+20]
		mov ebx,[esp+24]
		mov edi,[esp+28]
		mov ebp,[esp+32]
		mov edx,[esp+40]
		lea esi,[eax+ebx*2]
		pxor xmm4,xmm4 // sum
		pxor xmm5,xmm5 // sumsq
		pxor xmm6,xmm6
yloop:
		xor ecx,ecx
xloop:
		movq xmm0,QWORD PTR[eax+ecx]
		movq xmm1,QWORD PTR[esi+ecx]
		movdqa xmm2,xmm0
		movdqa xmm3,xmm1
		punpcklbw xmm0,xmm6
		punpcklbw xmm1,xmm6
		psadbw xmm2,xmm6
		psadbw xmm3,xmm6
		movdqa [edx],xmm0
		movdqa [edx+edi*2],xmm1
		pmaddwd xmm0,xmm0
		pmaddwd xmm1,xmm1
		paddd xmm4,xmm2
		paddd xmm5,xmm0
		paddd xmm4,xmm3
		paddd xmm5,xmm1
		add ecx,8
		add edx,16
		cmp ecx,edi
		jl xloop
		lea eax,[eax+ebx*4]
		lea esi,[esi+ebx*4]
		lea edx,[edx+edi*2]
		sub ebp,2
		jnz yloop
		movhlps xmm1,xmm5
		mov eax,[esp+32]
		paddd xmm5,xmm1
		mul edi
		pshuflw xmm1,xmm5,14
		cvtsi2ss xmm7,eax
		paddd xmm5,xmm1
		rcpss xmm7,xmm7 // scale
		cvtdq2ps xmm4,xmm4
		cvtdq2ps xmm5,xmm5
		mov eax,[esp+36]
		mulss xmm4,xmm7 // mean
		mulss xmm5,xmm7
		movss [eax],xmm4
		mulss xmm4,xmm4
		subss xmm5,xmm4 // var
		comiss xmm5,flt_epsilon_sse
		jbe novarjmp
		rsqrtss xmm5,xmm5 // 1.0/std
		rcpss xmm4,xmm5 // std
		movss [eax+4],xmm4
		movss [eax+8],xmm5
		jmp finish
novarjmp:
		movss [eax+4],xmm6
		movss [eax+8],xmm6
finish:
		movss [eax+12],xmm6
		pop esi
		pop edi
		pop ebx
		pop ebp
		ret
	}
}

__declspec(naked) void dotProd_m32_m16_SSE2(const float *data, const float *weights, 
	float *vals, const int n, const int len, const float *istd)
{
	__asm 
	{
		push edi
		push esi
		push ebx
		mov edi,[esp+20]//weights
		mov eax,[esp+24]//vals
		mov ebx,[esp+28]//n = # of weight vectors
		mov esi,[esp+32]//len = len of vectors
nloop:
		mov ecx,[esp+16]//data
		xorps xmm0,xmm0
		xorps xmm1,xmm1
		xorps xmm2,xmm2
		xorps xmm3,xmm3
		mov edx,esi
lloop:
		movaps xmm4,[ecx]
		movaps xmm5,xmm4
		movaps xmm6,xmm4
		movaps xmm7,xmm4
		mulps xmm4,[edi]
		mulps xmm5,[edi+16]
		mulps xmm6,[edi+32]
		mulps xmm7,[edi+48]
		addps xmm0,xmm4
		addps xmm1,xmm5
		addps xmm2,xmm6
		addps xmm3,xmm7
		movaps xmm4,[ecx+16]
		movaps xmm5,xmm4
		movaps xmm6,xmm4
		movaps xmm7,xmm4
		mulps xmm4,[edi+64]
		mulps xmm5,[edi+80]
		mulps xmm6,[edi+96]
		mulps xmm7,[edi+112]
		addps xmm0,xmm4
		addps xmm1,xmm5
		addps xmm2,xmm6
		addps xmm3,xmm7
		movaps xmm4,[ecx+32]
		movaps xmm5,xmm4
		movaps xmm6,xmm4
		movaps xmm7,xmm4
		mulps xmm4,[edi+128]
		mulps xmm5,[edi+144]
		mulps xmm6,[edi+160]
		mulps xmm7,[edi+176]
		addps xmm0,xmm4
		addps xmm1,xmm5
		addps xmm2,xmm6
		addps xmm3,xmm7
		movaps xmm4,[ecx+48]
		movaps xmm5,xmm4
		movaps xmm6,xmm4
		movaps xmm7,xmm4
		mulps xmm4,[edi+192]
		mulps xmm5,[edi+208]
		mulps xmm6,[edi+224]
		mulps xmm7,[edi+240]
		addps xmm0,xmm4
		addps xmm1,xmm5
		addps xmm2,xmm6
		addps xmm3,xmm7
		movaps xmm4,[ecx+64]
		movaps xmm5,xmm4
		movaps xmm6,xmm4
		movaps xmm7,xmm4
		mulps xmm4,[edi+256]
		mulps xmm5,[edi+272]
		mulps xmm6,[edi+288]
		mulps xmm7,[edi+304]
		addps xmm0,xmm4
		addps xmm1,xmm5
		addps xmm2,xmm6
		addps xmm3,xmm7
		movaps xmm4,[ecx+80]
		movaps xmm5,xmm4
		movaps xmm6,xmm4
		movaps xmm7,xmm4
		mulps xmm4,[edi+320]
		mulps xmm5,[edi+336]
		mulps xmm6,[edi+352]
		mulps xmm7,[edi+368]
		addps xmm0,xmm4
		addps xmm1,xmm5
		addps xmm2,xmm6
		addps xmm3,xmm7
		movaps xmm4,[ecx+96]
		movaps xmm5,xmm4
		movaps xmm6,xmm4
		movaps xmm7,xmm4
		mulps xmm4,[edi+384]
		mulps xmm5,[edi+400]
		mulps xmm6,[edi+416]
		mulps xmm7,[edi+432]
		addps xmm0,xmm4
		addps xmm1,xmm5
		addps xmm2,xmm6
		addps xmm3,xmm7
		movaps xmm4,[ecx+112]
		movaps xmm5,xmm4
		movaps xmm6,xmm4
		movaps xmm7,xmm4
		mulps xmm4,[edi+448]
		mulps xmm5,[edi+464]
		mulps xmm6,[edi+480]
		mulps xmm7,[edi+496]
		addps xmm0,xmm4
		addps xmm1,xmm5
		addps xmm2,xmm6
		addps xmm3,xmm7
		add ecx,128
		add edi,512
		sub edx,32
		jnz lloop
		movaps xmm4,xmm0
		movaps xmm5,xmm2
		unpcklpd xmm0,xmm1
		unpcklpd xmm2,xmm3
		unpckhpd xmm4,xmm1
		unpckhpd xmm5,xmm3
		addps xmm0,xmm4
		addps xmm2,xmm5
		movaps xmm6,xmm0
		shufps xmm0,xmm2,136
		shufps xmm6,xmm2,221
		addps xmm6,xmm0
		movaps [eax],xmm6
		add eax,16
		sub ebx,4
		jnz nloop
		mov ecx,[esp+36]//istd
		mov eax,[esp+24]//vals
		movss xmm7,[ecx]
		mov edx,[esp+28]//n
		shufps xmm7,xmm7,0
		xor ecx,ecx
aloop:
		movaps xmm0,[eax+ecx*4]
		movaps xmm1,[eax+ecx*4+16]
		movaps xmm2,[eax+ecx*4+32]
		movaps xmm3,[eax+ecx*4+48]
		mulps xmm0,xmm7
		mulps xmm1,xmm7
		mulps xmm2,xmm7
		mulps xmm3,xmm7
		addps xmm0,[edi+ecx*4]
		addps xmm1,[edi+ecx*4+16]
		addps xmm2,[edi+ecx*4+32]
		addps xmm3,[edi+ecx*4+48]
		movaps [eax+ecx*4],xmm0
		movaps [eax+ecx*4+16],xmm1
		movaps [eax+ecx*4+32],xmm2
		movaps [eax+ecx*4+48],xmm3
		add ecx,16
		sub edx,16
		jnz aloop
		pop ebx
		pop esi
		pop edi
		ret
	}
}

__declspec(naked) void dotProd_m48_m16_SSE2(const float *data, const float *weights, 
	float *vals, const int n, const int len, const float *istd)
{
	__asm 
	{
		push edi
		push esi
		push ebx
		mov edi,[esp+20]//weights
		mov eax,[esp+24]//vals
		mov ebx,[esp+28]//n = # of weight vectors
		mov esi,[esp+32]//len = len of vectors
nloop:
		mov ecx,[esp+16]//data
		xorps xmm0,xmm0
		xorps xmm1,xmm1
		xorps xmm2,xmm2
		xorps xmm3,xmm3
		mov edx,esi
lloop:
		movaps xmm4,[ecx]
		movaps xmm5,xmm4
		movaps xmm6,xmm4
		movaps xmm7,xmm4
		mulps xmm4,[edi]
		mulps xmm5,[edi+16]
		mulps xmm6,[edi+32]
		mulps xmm7,[edi+48]
		addps xmm0,xmm4
		addps xmm1,xmm5
		addps xmm2,xmm6
		addps xmm3,xmm7
		movaps xmm4,[ecx+16]
		movaps xmm5,xmm4
		movaps xmm6,xmm4
		movaps xmm7,xmm4
		mulps xmm4,[edi+64]
		mulps xmm5,[edi+80]
		mulps xmm6,[edi+96]
		mulps xmm7,[edi+112]
		addps xmm0,xmm4
		addps xmm1,xmm5
		addps xmm2,xmm6
		addps xmm3,xmm7
		movaps xmm4,[ecx+32]
		movaps xmm5,xmm4
		movaps xmm6,xmm4
		movaps xmm7,xmm4
		mulps xmm4,[edi+128]
		mulps xmm5,[edi+144]
		mulps xmm6,[edi+160]
		mulps xmm7,[edi+176]
		addps xmm0,xmm4
		addps xmm1,xmm5
		addps xmm2,xmm6
		addps xmm3,xmm7
		movaps xmm4,[ecx+48]
		movaps xmm5,xmm4
		movaps xmm6,xmm4
		movaps xmm7,xmm4
		mulps xmm4,[edi+192]
		mulps xmm5,[edi+208]
		mulps xmm6,[edi+224]
		mulps xmm7,[edi+240]
		addps xmm0,xmm4
		addps xmm1,xmm5
		addps xmm2,xmm6
		addps xmm3,xmm7
		movaps xmm4,[ecx+64]
		movaps xmm5,xmm4
		movaps xmm6,xmm4
		movaps xmm7,xmm4
		mulps xmm4,[edi+256]
		mulps xmm5,[edi+272]
		mulps xmm6,[edi+288]
		mulps xmm7,[edi+304]
		addps xmm0,xmm4
		addps xmm1,xmm5
		addps xmm2,xmm6
		addps xmm3,xmm7
		movaps xmm4,[ecx+80]
		movaps xmm5,xmm4
		movaps xmm6,xmm4
		movaps xmm7,xmm4
		mulps xmm4,[edi+320]
		mulps xmm5,[edi+336]
		mulps xmm6,[edi+352]
		mulps xmm7,[edi+368]
		addps xmm0,xmm4
		addps xmm1,xmm5
		addps xmm2,xmm6
		addps xmm3,xmm7
		movaps xmm4,[ecx+96]
		movaps xmm5,xmm4
		movaps xmm6,xmm4
		movaps xmm7,xmm4
		mulps xmm4,[edi+384]
		mulps xmm5,[edi+400]
		mulps xmm6,[edi+416]
		mulps xmm7,[edi+432]
		addps xmm0,xmm4
		addps xmm1,xmm5
		addps xmm2,xmm6
		addps xmm3,xmm7
		movaps xmm4,[ecx+112]
		movaps xmm5,xmm4
		movaps xmm6,xmm4
		movaps xmm7,xmm4
		mulps xmm4,[edi+448]
		mulps xmm5,[edi+464]
		mulps xmm6,[edi+480]
		mulps xmm7,[edi+496]
		addps xmm0,xmm4
		addps xmm1,xmm5
		addps xmm2,xmm6
		addps xmm3,xmm7
		movaps xmm4,[ecx+128]
		movaps xmm5,xmm4
		movaps xmm6,xmm4
		movaps xmm7,xmm4
		mulps xmm4,[edi+512]
		mulps xmm5,[edi+528]
		mulps xmm6,[edi+544]
		mulps xmm7,[edi+560]
		addps xmm0,xmm4
		addps xmm1,xmm5
		addps xmm2,xmm6
		addps xmm3,xmm7
		movaps xmm4,[ecx+144]
		movaps xmm5,xmm4
		movaps xmm6,xmm4
		movaps xmm7,xmm4
		mulps xmm4,[edi+576]
		mulps xmm5,[edi+592]
		mulps xmm6,[edi+608]
		mulps xmm7,[edi+624]
		addps xmm0,xmm4
		addps xmm1,xmm5
		addps xmm2,xmm6
		addps xmm3,xmm7
		movaps xmm4,[ecx+160]
		movaps xmm5,xmm4
		movaps xmm6,xmm4
		movaps xmm7,xmm4
		mulps xmm4,[edi+640]
		mulps xmm5,[edi+656]
		mulps xmm6,[edi+672]
		mulps xmm7,[edi+688]
		addps xmm0,xmm4
		addps xmm1,xmm5
		addps xmm2,xmm6
		addps xmm3,xmm7
		movaps xmm4,[ecx+176]
		movaps xmm5,xmm4
		movaps xmm6,xmm4
		movaps xmm7,xmm4
		mulps xmm4,[edi+704]
		mulps xmm5,[edi+720]
		mulps xmm6,[edi+736]
		mulps xmm7,[edi+752]
		addps xmm0,xmm4
		addps xmm1,xmm5
		addps xmm2,xmm6
		addps xmm3,xmm7
		add ecx,192
		add edi,768
		sub edx,48
		jnz lloop
		movaps xmm4,xmm0
		movaps xmm5,xmm2
		unpcklpd xmm0,xmm1
		unpcklpd xmm2,xmm3
		unpckhpd xmm4,xmm1
		unpckhpd xmm5,xmm3
		addps xmm0,xmm4
		addps xmm2,xmm5
		movaps xmm6,xmm0
		shufps xmm0,xmm2,136
		shufps xmm6,xmm2,221
		addps xmm6,xmm0
		movaps [eax],xmm6
		add eax,16
		sub ebx,4
		jnz nloop
		mov ecx,[esp+36]//istd
		mov eax,[esp+24]//vals
		movss xmm7,[ecx]
		mov edx,[esp+28]//n
		shufps xmm7,xmm7,0
		xor ecx,ecx
aloop:
		movaps xmm0,[eax+ecx*4]
		movaps xmm1,[eax+ecx*4+16]
		movaps xmm2,[eax+ecx*4+32]
		movaps xmm3,[eax+ecx*4+48]
		mulps xmm0,xmm7
		mulps xmm1,xmm7
		mulps xmm2,xmm7
		mulps xmm3,xmm7
		addps xmm0,[edi+ecx*4]
		addps xmm1,[edi+ecx*4+16]
		addps xmm2,[edi+ecx*4+32]
		addps xmm3,[edi+ecx*4+48]
		movaps [eax+ecx*4],xmm0
		movaps [eax+ecx*4+16],xmm1
		movaps [eax+ecx*4+32],xmm2
		movaps [eax+ecx*4+48],xmm3
		add ecx,16
		sub edx,16
		jnz aloop
		pop ebx
		pop esi
		pop edi
		ret
	}
}

__declspec(naked) void dotProd_m32_m16_i16_SSE2(const float *dataf, const float *weightsf, 
	float *vals, const int n, const int len, const float *istd)
{
	__asm 
	{
		push edi
		push esi
		push ebx
		mov edi,[esp+20]//weights
		mov eax,[esp+24]//vals
		mov ebx,[esp+28]//n = # of weight vectors
		mov esi,[esp+32]//len = len of vectors
nloop:
		mov ecx,[esp+16]//data
		pxor xmm0,xmm0
		pxor xmm1,xmm1
		pxor xmm2,xmm2
		pxor xmm3,xmm3
		mov edx,esi
lloop:
		movdqa xmm4,[ecx]
		movdqa xmm5,xmm4
		movdqa xmm6,xmm4
		movdqa xmm7,xmm4
		pmaddwd xmm4,[edi]
		pmaddwd xmm5,[edi+16]
		pmaddwd xmm6,[edi+32]
		pmaddwd xmm7,[edi+48]
		paddd xmm0,xmm4
		paddd xmm1,xmm5
		paddd xmm2,xmm6
		paddd xmm3,xmm7
		movdqa xmm4,[ecx+16]
		movdqa xmm5,xmm4
		movdqa xmm6,xmm4
		movdqa xmm7,xmm4
		pmaddwd xmm4,[edi+64]
		pmaddwd xmm5,[edi+80]
		pmaddwd xmm6,[edi+96]
		pmaddwd xmm7,[edi+112]
		paddd xmm0,xmm4
		paddd xmm1,xmm5
		paddd xmm2,xmm6
		paddd xmm3,xmm7
		movdqa xmm4,[ecx+32]
		movdqa xmm5,xmm4
		movdqa xmm6,xmm4
		movdqa xmm7,xmm4
		pmaddwd xmm4,[edi+128]
		pmaddwd xmm5,[edi+144]
		pmaddwd xmm6,[edi+160]
		pmaddwd xmm7,[edi+176]
		paddd xmm0,xmm4
		paddd xmm1,xmm5
		paddd xmm2,xmm6
		paddd xmm3,xmm7
		movdqa xmm4,[ecx+48]
		movdqa xmm5,xmm4
		movdqa xmm6,xmm4
		movdqa xmm7,xmm4
		pmaddwd xmm4,[edi+192]
		pmaddwd xmm5,[edi+208]
		pmaddwd xmm6,[edi+224]
		pmaddwd xmm7,[edi+240]
		paddd xmm0,xmm4
		paddd xmm1,xmm5
		paddd xmm2,xmm6
		paddd xmm3,xmm7
		add ecx,64
		add edi,256
		sub edx,32
		jnz lloop
		movdqa xmm4,xmm0
		movdqa xmm5,xmm2
		punpcklqdq xmm0,xmm1
		punpcklqdq xmm2,xmm3
		punpckhqdq xmm4,xmm1
		punpckhqdq xmm5,xmm3
		paddd xmm0,xmm4
		paddd xmm2,xmm5
		movdqa xmm6,xmm0
		shufps xmm0,xmm2,136
		shufps xmm6,xmm2,221
		paddd xmm6,xmm0
		movdqa [eax],xmm6
		add eax,16
		sub ebx,4
		jnz nloop
		mov ecx,[esp+36]//istd
		mov eax,[esp+24]//vals
		movss xmm7,[ecx]
		mov edx,[esp+28]//n
		pshufd xmm7,xmm7,0
		xor ecx,ecx
aloop:
		movdqa xmm0,[eax+ecx*4]
		movdqa xmm1,[eax+ecx*4+16]
		movdqa xmm2,[eax+ecx*4+32]
		movdqa xmm3,[eax+ecx*4+48]
		cvtdq2ps xmm0,xmm0
		cvtdq2ps xmm1,xmm1
		cvtdq2ps xmm2,xmm2
		cvtdq2ps xmm3,xmm3
		mulps xmm0,[edi+ecx*8]
		mulps xmm1,[edi+ecx*8+32]
		mulps xmm2,[edi+ecx*8+64]
		mulps xmm3,[edi+ecx*8+96]
		mulps xmm0,xmm7
		mulps xmm1,xmm7
		mulps xmm2,xmm7
		mulps xmm3,xmm7
		addps xmm0,[edi+ecx*8+16]
		addps xmm1,[edi+ecx*8+48]
		addps xmm2,[edi+ecx*8+80]
		addps xmm3,[edi+ecx*8+112]
		movaps [eax+ecx*4],xmm0
		movaps [eax+ecx*4+16],xmm1
		movaps [eax+ecx*4+32],xmm2
		movaps [eax+ecx*4+48],xmm3
		add ecx,16
		sub edx,16
		jnz aloop
		pop ebx
		pop esi
		pop edi
		ret
	}
}

__declspec(naked) void dotProd_m48_m16_i16_SSE2(const float *dataf, const float *weightsf, 
	float *vals, const int n, const int len, const float *istd)
{
	__asm 
	{
		push edi
		push esi
		push ebx
		mov edi,[esp+20]//weights
		mov eax,[esp+24]//vals
		mov ebx,[esp+28]//n = # of weight vectors
		mov esi,[esp+32]//len = len of vectors
nloop:
		mov ecx,[esp+16]//data
		pxor xmm0,xmm0
		pxor xmm1,xmm1
		pxor xmm2,xmm2
		pxor xmm3,xmm3
		mov edx,esi
lloop:
		movdqa xmm4,[ecx]
		movdqa xmm5,xmm4
		movdqa xmm6,xmm4
		movdqa xmm7,xmm4
		pmaddwd xmm4,[edi]
		pmaddwd xmm5,[edi+16]
		pmaddwd xmm6,[edi+32]
		pmaddwd xmm7,[edi+48]
		paddd xmm0,xmm4
		paddd xmm1,xmm5
		paddd xmm2,xmm6
		paddd xmm3,xmm7
		movdqa xmm4,[ecx+16]
		movdqa xmm5,xmm4
		movdqa xmm6,xmm4
		movdqa xmm7,xmm4
		pmaddwd xmm4,[edi+64]
		pmaddwd xmm5,[edi+80]
		pmaddwd xmm6,[edi+96]
		pmaddwd xmm7,[edi+112]
		paddd xmm0,xmm4
		paddd xmm1,xmm5
		paddd xmm2,xmm6
		paddd xmm3,xmm7
		movdqa xmm4,[ecx+32]
		movdqa xmm5,xmm4
		movdqa xmm6,xmm4
		movdqa xmm7,xmm4
		pmaddwd xmm4,[edi+128]
		pmaddwd xmm5,[edi+144]
		pmaddwd xmm6,[edi+160]
		pmaddwd xmm7,[edi+176]
		paddd xmm0,xmm4
		paddd xmm1,xmm5
		paddd xmm2,xmm6
		paddd xmm3,xmm7
		movdqa xmm4,[ecx+48]
		movdqa xmm5,xmm4
		movdqa xmm6,xmm4
		movdqa xmm7,xmm4
		pmaddwd xmm4,[edi+192]
		pmaddwd xmm5,[edi+208]
		pmaddwd xmm6,[edi+224]
		pmaddwd xmm7,[edi+240]
		paddd xmm0,xmm4
		paddd xmm1,xmm5
		paddd xmm2,xmm6
		paddd xmm3,xmm7
		movdqa xmm4,[ecx+64]
		movdqa xmm5,xmm4
		movdqa xmm6,xmm4
		movdqa xmm7,xmm4
		pmaddwd xmm4,[edi+256]
		pmaddwd xmm5,[edi+272]
		pmaddwd xmm6,[edi+288]
		pmaddwd xmm7,[edi+304]
		paddd xmm0,xmm4
		paddd xmm1,xmm5
		paddd xmm2,xmm6
		paddd xmm3,xmm7
		movdqa xmm4,[ecx+80]
		movdqa xmm5,xmm4
		movdqa xmm6,xmm4
		movdqa xmm7,xmm4
		pmaddwd xmm4,[edi+320]
		pmaddwd xmm5,[edi+336]
		pmaddwd xmm6,[edi+352]
		pmaddwd xmm7,[edi+368]
		paddd xmm0,xmm4
		paddd xmm1,xmm5
		paddd xmm2,xmm6
		paddd xmm3,xmm7
		add ecx,96
		add edi,384
		sub edx,48
		jnz lloop
		movdqa xmm4,xmm0
		movdqa xmm5,xmm2
		punpcklqdq xmm0,xmm1
		punpcklqdq xmm2,xmm3
		punpckhqdq xmm4,xmm1
		punpckhqdq xmm5,xmm3
		paddd xmm0,xmm4
		paddd xmm2,xmm5
		movdqa xmm6,xmm0
		shufps xmm0,xmm2,136
		shufps xmm6,xmm2,221
		paddd xmm6,xmm0
		movdqa [eax],xmm6
		add eax,16
		sub ebx,4
		jnz nloop
		mov ecx,[esp+36]//istd
		mov eax,[esp+24]//vals
		movss xmm7,[ecx]
		mov edx,[esp+28]//n
		pshufd xmm7,xmm7,0
		xor ecx,ecx
aloop:
		movdqa xmm0,[eax+ecx*4]
		movdqa xmm1,[eax+ecx*4+16]
		movdqa xmm2,[eax+ecx*4+32]
		movdqa xmm3,[eax+ecx*4+48]
		cvtdq2ps xmm0,xmm0
		cvtdq2ps xmm1,xmm1
		cvtdq2ps xmm2,xmm2
		cvtdq2ps xmm3,xmm3
		mulps xmm0,[edi+ecx*8]
		mulps xmm1,[edi+ecx*8+32]
		mulps xmm2,[edi+ecx*8+64]
		mulps xmm3,[edi+ecx*8+96]
		mulps xmm0,xmm7
		mulps xmm1,xmm7
		mulps xmm2,xmm7
		mulps xmm3,xmm7
		addps xmm0,[edi+ecx*8+16]
		addps xmm1,[edi+ecx*8+48]
		addps xmm2,[edi+ecx*8+80]
		addps xmm3,[edi+ecx*8+112]
		movaps [eax+ecx*4],xmm0
		movaps [eax+ecx*4+16],xmm1
		movaps [eax+ecx*4+32],xmm2
		movaps [eax+ecx*4+48],xmm3
		add ecx,16
		sub edx,16
		jnz aloop
		pop ebx
		pop esi
		pop edi
		ret
	}
}

__declspec(align(16)) const float exp_lo[4] = { -80.0f, -80.0f, -80.0f, -80.0f };
__declspec(align(16)) const float exp_hi[4] = { +80.0f, +80.0f, +80.0f, +80.0f };

// exp from:  A Fast, Compact Approximation of the Exponential Function (1998)
//            Nicol N. Schraudolph

__declspec(align(16)) const float e0_mult[4] = { // (1.0/ln(2))*(2^23)
	12102203.161561486f, 12102203.161561486f, 12102203.161561486f, 12102203.161561486f };
__declspec(align(16)) const float e0_bias[4] = { // (2^23)*127.0-486411.0
	1064866805.0f, 1064866805.0f, 1064866805.0f, 1064866805.0f };

void e0_m16_C(float *s, const int n)
{
	for (int i=0; i<n; ++i)
	{
		const int t = (int)(max(min(s[i],exp_hi[0]),exp_lo[0])*e0_mult[0]+e0_bias[0]);
		s[i] = (*((float*)&t));
	}
}

__declspec(naked) void e0_m16_SSE2(float *s, const int n)
{
	__asm
	{
		mov eax,[esp+4]
		mov ecx,[esp+8]
eloop16:
		movaps xmm0,[eax]
		movaps xmm1,[eax+16]
		movaps xmm2,[eax+32]
		movaps xmm3,[eax+48]
		minps xmm0,exp_hi
		minps xmm1,exp_hi
		minps xmm2,exp_hi
		minps xmm3,exp_hi
		maxps xmm0,exp_lo
		maxps xmm1,exp_lo
		maxps xmm2,exp_lo
		maxps xmm3,exp_lo
		mulps xmm0,e0_mult
		mulps xmm1,e0_mult
		mulps xmm2,e0_mult
		mulps xmm3,e0_mult
		addps xmm0,e0_bias
		addps xmm1,e0_bias
		addps xmm2,e0_bias
		addps xmm3,e0_bias
		cvtps2dq xmm0,xmm0
		cvtps2dq xmm1,xmm1
		cvtps2dq xmm2,xmm2
		cvtps2dq xmm3,xmm3
		movaps [eax],xmm0
		movaps [eax+16],xmm1
		movaps [eax+32],xmm2
		movaps [eax+48],xmm3
		add eax,64
		sub ecx,16
		jnz eloop16
		ret
	}
}

// exp from Loren Merritt

_declspec(align(16)) const float e1_scale[4] = { // 1/ln(2)
	1.4426950409f, 1.4426950409f, 1.4426950409f, 1.4426950409f };
_declspec(align(16)) const float e1_bias[4] = { // 3<<22
	12582912.0f, 12582912.0f, 12582912.0f, 12582912.0f };
_declspec(align(16)) const float e1_c0[4] = { 1.00035f, 1.00035f, 1.00035f, 1.00035f };
_declspec(align(16)) const float e1_c1[4] = { 0.701277797f, 0.701277797f, 0.701277797f, 0.701277797f };
_declspec(align(16)) const float e1_c2[4] = { 0.237348593f, 0.237348593f, 0.237348593f, 0.237348593f };

void e1_m16_C(float *s, const int n)
{
	for (int q=0; q<n; ++q)
	{
		float x = max(min(s[q],exp_hi[0]),exp_lo[0])*e1_scale[0];
		int i = (int)(x + 128.5f) - 128;
		x -= i;
		x = e1_c0[0] + e1_c1[0]*x + e1_c2[0]*x*x;
		i = (i+127)<<23;
		s[q] = x * *((float*)&i);
	}
}

__declspec(naked) void e1_m16_SSE2(float *s, const int n)
{
	__asm
	{
		mov eax,[esp+4]
		mov ecx,[esp+8]
eloop8:
		movaps xmm0,[eax]
		movaps xmm4,[eax+16]
		minps xmm0,exp_hi
		minps xmm4,exp_hi
		maxps xmm0,exp_lo
		maxps xmm4,exp_lo
		mulps xmm0,e1_scale
		mulps xmm4,e1_scale
		movaps xmm1,xmm0
		movaps xmm5,xmm4
		addps xmm0,e1_bias
		addps xmm4,e1_bias
		movaps xmm2,xmm0
		movaps xmm6,xmm4
		subps xmm0,e1_bias
		subps xmm4,e1_bias
		pslld xmm2,23
		pslld xmm6,23
		subps xmm1,xmm0
		subps xmm5,xmm4
		movaps xmm0,xmm1
		movaps xmm4,xmm5
		mulps xmm1,xmm1
		mulps xmm5,xmm5
		mulps xmm0,e1_c1
		mulps xmm4,e1_c1
		mulps xmm1,e1_c2
		mulps xmm5,e1_c2
		addps xmm0,e1_c0
		addps xmm4,e1_c0
		addps xmm0,xmm1
		addps xmm4,xmm5
		paddd xmm0,xmm2
		paddd xmm4,xmm6
		movaps [eax],xmm0
		movaps [eax+16],xmm4
		add eax,32
		sub ecx,8
		jnz eloop8
		ret
	}
}

void e2_m16_C(float *s, const int n)
{
	for (int i=0; i<n; ++i)
		s[i] = expf(max(min(s[i],exp_hi[0]),exp_lo[0]));
}

// exp from Intel Approximate Math (AM) Library

__declspec(align(16)) const float am_0p5[4] = { 0.5f, 0.5f, 0.5f, 0.5f };
__declspec(align(16)) const float am_1[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
__declspec(align(16)) const float exp_rln2[4] = { 1.442695041f, 1.442695041f, 1.442695041f, 1.442695041f };
__declspec(align(16)) const float exp_p0[4] = { 1.261771931e-4f, 1.261771931e-4f, 1.261771931e-4f, 1.261771931e-4f };
__declspec(align(16)) const float exp_p1[4] = { 3.029944077e-2f, 3.029944077e-2f, 3.029944077e-2f, 3.029944077e-2f };
__declspec(align(16)) const float exp_q0[4] = { 3.001985051e-6f, 3.001985051e-6f, 3.001985051e-6f, 3.001985051e-6f };
__declspec(align(16)) const float exp_q1[4] = { 2.524483403e-3f, 2.524483403e-3f, 2.524483403e-3f, 2.524483403e-3f };
__declspec(align(16)) const float exp_q2[4] = { 2.272655482e-1f, 2.272655482e-1f, 2.272655482e-1f, 2.272655482e-1f };
__declspec(align(16)) const float exp_q3[4] = { 2.000000000f, 2.000000000f, 2.000000000f, 2.000000000f };
__declspec(align(16)) const float exp_c1[4] = { 6.931457520e-1f, 6.931457520e-1f, 6.931457520e-1f, 6.931457520e-1f };
__declspec(align(16)) const float exp_c2[4] = { 1.428606820e-6f, 1.428606820e-6f, 1.428606820e-6f, 1.428606820e-6f };
__declspec(align(16)) const int epi32_1[4] = { 1, 1, 1, 1 };
__declspec(align(16)) const int epi32_0x7f[4] = { 0x7f, 0x7f, 0x7f, 0x7f };

__declspec(naked) void e2_m16_SSE2(float *s, const int n)
{
	__asm
	{
		mov eax,[esp+4]
		mov ecx,[esp+8]
eloop4:
		movaps xmm0,[eax]
		minps xmm0,exp_hi
		maxps xmm0,exp_lo
		movaps xmm1,exp_rln2
		mulps xmm1,xmm0
		xorps xmm2,xmm2
		addps xmm1,am_0p5
		cmpnltps xmm2,xmm1
		pand xmm2,epi32_1
		cvttps2dq xmm1,xmm1
		movaps xmm4,exp_c2
		psubd xmm1,xmm2
		movaps xmm5,exp_c1
		cvtdq2ps xmm3,xmm1
		mulps xmm4,xmm3
		mulps xmm5,xmm3
		movaps xmm6,exp_q0
		subps xmm0,xmm4
		movaps xmm4,exp_p0
		subps xmm0,xmm5
		paddd xmm1,epi32_0x7f
		movaps xmm2,xmm0
		mulps xmm0,xmm0
		mulps xmm6,xmm0
		mulps xmm4,xmm0
		addps xmm6,exp_q1
		addps xmm4,exp_p1
		mulps xmm6,xmm0
		mulps xmm4,xmm0
		addps xmm6,exp_q2
		mulps xmm4,xmm2
		mulps xmm6,xmm0
		movaps xmm0,am_1
		addps xmm2,xmm4
		addps xmm6,exp_q3
		pslld xmm1,23
		subps xmm6,xmm2
		rcpps xmm6,xmm6
		mulps xmm2,xmm6
		addps xmm2,xmm2
		addps xmm0,xmm2
		mulps xmm0,xmm1
		movaps [eax],xmm0
		add eax,16
		sub ecx,4
		jnz eloop4
		ret
	}
}

__declspec(align(16)) const float min_weight_sum[4] = { 1e-10f, 1e-10f, 1e-10f, 1e-10f };
__declspec(align(16)) const float five_f[4] = { 5.0f, 5.0f, 5.0f, 5.0f };

void weightedAvgElliottMul5_m16_C(const float *w, const int n, float *mstd)
{
	float vsum = 0.0f, wsum = 0.0f;
	for (int i=0; i<n; ++i)
	{
		vsum += w[i]*(w[n+i]/(1.0f+fabsf(w[n+i])));
		wsum += w[i];
	}
	if (wsum > min_weight_sum[0])
		mstd[3] += ((5.0f*vsum)/wsum)*mstd[1]+mstd[0];
	else
		mstd[3] += mstd[0];
}

__declspec(naked) void weightedAvgElliottMul5_m16_SSE2(const float *w, const int n, float *mstd)
{
	__asm
	{
		push edi
		mov eax,[esp+8]
		mov ecx,[esp+12]
		lea edx,[eax+ecx*4]
		xor edi,edi
		xorps xmm0,xmm0 // sum w
		xorps xmm1,xmm1 // sum w*v
nloop:
		movaps xmm2,[eax+edi*4]
		movaps xmm3,[eax+edi*4+16]
		movaps xmm4,[edx+edi*4]
		movaps xmm5,[edx+edi*4+16]
		addps xmm0,xmm2
		movaps xmm6,xmm4
		movaps xmm7,xmm5
		addps xmm0,xmm3
		andps xmm4,sign_bits_f
		andps xmm5,sign_bits_f
		addps xmm4,ones_f
		addps xmm5,ones_f
		rcpps xmm4,xmm4
		rcpps xmm5,xmm5
		mulps xmm6,xmm4
		mulps xmm7,xmm5
		mulps xmm6,xmm2
		mulps xmm7,xmm3
		addps xmm1,xmm6
		addps xmm1,xmm7
		movaps xmm2,[eax+edi*4+32]
		movaps xmm3,[eax+edi*4+48]
		movaps xmm4,[edx+edi*4+32]
		movaps xmm5,[edx+edi*4+48]
		addps xmm0,xmm2
		movaps xmm6,xmm4
		movaps xmm7,xmm5
		addps xmm0,xmm3
		andps xmm4,sign_bits_f
		andps xmm5,sign_bits_f
		addps xmm4,ones_f
		addps xmm5,ones_f
		rcpps xmm4,xmm4
		rcpps xmm5,xmm5
		mulps xmm6,xmm4
		mulps xmm7,xmm5
		mulps xmm6,xmm2
		mulps xmm7,xmm3
		addps xmm1,xmm6
		addps xmm1,xmm7
		add edi,16
		sub ecx,16
		jnz nloop
		movhlps xmm2,xmm0
		movhlps xmm3,xmm1
		addps xmm0,xmm2
		addps xmm1,xmm3
		pshuflw xmm2,xmm0,14
		pshuflw xmm3,xmm1,14
		addss xmm0,xmm2
		addss xmm1,xmm3
		comiss xmm0,min_weight_sum
		jbe nodiv
		mulss xmm1,five_f
		rcpss xmm0,xmm0
		mulss xmm1,xmm0
		jmp finish
nodiv:
		xorps xmm1,xmm1
finish:
		mov eax,[esp+16]
		mulss xmm1,[eax+4]
		addss xmm1,[eax]
		addss xmm1,[eax+12]
		movss [eax+12],xmm1
		pop edi
		ret
	}
}

__declspec(align(16)) const float sse_half[4] = { 0.5f, 0.5f, 0.5f, 0.5f };

void inline castScale_SSE(const float *val, const float *scale, unsigned char *dstp)
{
	__asm
	{
		mov ecx,val
		mov eax,scale
		movss xmm0,[ecx+12]
		mulss xmm0,[eax]
		addss xmm0,sse_half
		cvttss2si eax,xmm0
		mov ecx,dstp
		cmp eax,255
		jl b255
		mov eax,255
		jmp finish
b255:
		cmp eax,0
		jge finish
		xor eax,eax
finish:
		mov byte ptr[ecx],al
	}
}

void evalFunc_1(void *ps)
{
	PS_INFO *pss = (PS_INFO*)ps;
	float *input = pss->input;
	float *temp = pss->temp;
	float **weights1 = pss->weights1;
	const int opt = pss->opt;
	const int qual = pss->qual;
	const int asize = pss->asize;
	const int nns = pss->nns;
	const int xdia = pss->xdia;
	const int xdiad2m1 = (xdia>>1)-1;
	const int ydia = pss->ydia;
	const int fapprox = pss->fapprox;
	const float scale = 1.0f/(float)qual;
	void (*extract)(const unsigned char*, const int, const int, const int, float*, float*);
	void (*dotProd)(const float*, const float*, float*, const int, const int, const float*);
	void (*expf)(float *, const int);
	void (*wae5)(const float*, const int, float*);
	if (opt == 1) wae5 = weightedAvgElliottMul5_m16_C;
	else wae5 = weightedAvgElliottMul5_m16_SSE2;
	if (fapprox&2) // use int16 dot products
	{
		if (opt == 1) extract = extract_m8_i16_C;
		else extract = extract_m8_i16_SSE2;
		if (opt == 1) dotProd = dotProdS_C;
		else dotProd = (asize%48) ? dotProd_m32_m16_i16_SSE2 : dotProd_m48_m16_i16_SSE2;
	}
	else // use float dot products
	{
		if (opt == 1) extract = extract_m8_C;
		else extract = extract_m8_SSE2;
		if (opt == 1) dotProd = dotProd_C;
		else dotProd = (asize%48) ? dotProd_m32_m16_SSE2 : dotProd_m48_m16_SSE2;
	}
	if ((fapprox&12) == 0) // use slow exp
	{
		if (opt == 1) expf = e2_m16_C;
		else expf = e2_m16_SSE2;
	}
	else if ((fapprox&12) == 4) // use faster exp
	{
		if (opt == 1) expf = e1_m16_C;
		else expf = e1_m16_SSE2;
	}
	else // use fastest exp
	{
		if (opt == 1) expf = e0_m16_C;
		else expf = e0_m16_SSE2;
	}
	for (int b=0; b<3; ++b)
	{
		if ((b == 0 && !pss->Y) || 
			(b == 1 && !pss->U) ||
			(b == 2 && !pss->V))
			continue;
		const unsigned char *srcp = pss->srcp[b];
		const int src_pitch = pss->src_pitch[b];
		const int width = pss->width[b];
		unsigned char *dstp = pss->dstp[b];
		const int dst_pitch = pss->dst_pitch[b];
		const int ystart = pss->sheight2[b];
		const int ystop = pss->eheight2[b];
		srcp += (ystart+6)*src_pitch;
		dstp += ystart*dst_pitch-32;
		const unsigned char *srcpp = srcp-(ydia-1)*src_pitch-xdiad2m1;
		for (int y=ystart; y<ystop; y+=2)
		{
			for (int x=32; x<width-32; ++x)
			{
				if (dstp[x] != 255)
					continue;
				float mstd[4];
				extract(srcpp+x,src_pitch,xdia,ydia,mstd,input);
				for (int i=0; i<qual; ++i)
				{
					dotProd(input,weights1[i],temp,nns*2,asize,mstd+2);
					expf(temp,nns);
					wae5(temp,nns,mstd);
				}
				if (opt > 1)
					castScale_SSE(mstd,&scale,dstp+x);
				else
					dstp[x] = min(max((int)(mstd[3]*scale+0.5f),0),255);
			}
			srcpp += src_pitch*2;
			dstp += dst_pitch*2;
		}
	}
}

unsigned __stdcall threadPool(void *ps)
{
	const PS_INFO *pss = (PS_INFO*)ps;
	while (true)
	{
		WaitForSingleObject(pss->nextJob,INFINITE);
		if (pss->type < 0)
			return 0;
		if (pss->type == 0)
			evalFunc_0(ps);
		else
			evalFunc_1(ps);
		ResetEvent(pss->nextJob);
		SetEvent(pss->jobFinished);
	}
}

AVSValue __cdecl Create_nnedi3(AVSValue args, void* user_data, IScriptEnvironment* env)
{
	if (!args[0].IsClip())
		env->ThrowError("nnedi3:  arg 0 must be a clip!");
	VideoInfo vi = args[0].AsClip()->GetVideoInfo();
	if (!vi.IsYV12() && !vi.IsYUY2() && !vi.IsRGB24())
		env->ThrowError("nnedi3:  only YV12, YUY2, and RGB24 input are supported!");
	const bool dh = args[2].AsBool(false);
	if ((vi.height&1) && !dh)
		env->ThrowError("nnedi3:  height must be mod 2 when dh=false (%d)!", vi.height);
	return new nnedi3(args[0].AsClip(),args[1].AsInt(-1),args[2].AsBool(false),
		args[3].AsBool(true),args[4].AsBool(true),args[5].AsBool(true),
		args[6].AsInt(6),args[7].AsInt(1),args[8].AsInt(1),args[9].AsInt(0),
		args[10].AsInt(2),args[11].AsInt(0),args[12].AsInt(0),args[13].AsInt(15),env);
}

AVSValue __cdecl Create_nnedi3_rpow2(AVSValue args, void* user_data, IScriptEnvironment *env)
{
	if (!args[0].IsClip())
		env->ThrowError("nnedi3_rpow2:  arg 0 must be a clip!");
	VideoInfo vi = args[0].AsClip()->GetVideoInfo();
	if (!vi.IsYV12() && !vi.IsYUY2() && !vi.IsRGB24())
		env->ThrowError("nnedi3_rpow2:  only YV12, YUY2, and RGB24 input are supported!");
	if (vi.IsYUY2() && (vi.width&3))
		env->ThrowError("nnedi3_rpow2:  for yuy2 input width must be mod 4 (%d)!", vi.width);
	const int rfactor = args[1].AsInt(-1);
	const int nsize = args[2].AsInt(0);
	const int nns = args[3].AsInt(3);
	const int qual = args[4].AsInt(1);
	const int etype = args[5].AsInt(0);
	const int pscrn = args[6].AsInt(2);
	const char *cshift = args[7].AsString("");
	const int fwidth = args[8].IsInt() ? args[8].AsInt() : rfactor*vi.width;
	const int fheight = args[9].IsInt() ? args[9].AsInt() : rfactor*vi.height;
	const float ep0 = args[10].IsFloat() ? args[10].AsFloat() : -FLT_MAX;
	const float ep1 = args[11].IsFloat() ? args[11].AsFloat() : -FLT_MAX;
	const int threads = args[12].AsInt(0);
	const int opt = args[13].AsInt(0);
	const int fapprox = args[14].AsInt(15);
	if (rfactor < 2 || rfactor > 1024)
		env->ThrowError("nnedi3_rpow2:  2 <= rfactor <= 1024, and rfactor be a power of 2!\n");
	int rf = 1, ct = 0;
	while (rf < rfactor)
	{
		rf *= 2;
		++ct;
	}
	if (rf != rfactor)
		env->ThrowError("nnedi3_rpow2:  2 <= rfactor <= 1024, and rfactor be a power of 2!\n");
	if (nsize < 0 || nsize >= NUM_NSIZE)
		env->ThrowError("nnedi3_rpow2:  nsize must be in [0,%d]!\n", NUM_NSIZE-1);
	if (nns < 0 || nns >= NUM_NNS)
		env->ThrowError("nnedi3_rpow2:  nns must be in [0,%d]!\n", NUM_NNS-1);
	if (qual < 1 || qual > 2)
		env->ThrowError("nnedi3_rpow2:  qual must be set to 1 or 2!\n");
	if (threads < 0 || threads > 16)
		env->ThrowError("nnedi3_rpow2:  0 <= threads <= 16!\n");
	if (opt < 0 || opt > 2)
		env->ThrowError("nnedi3_rpow2:  opt must be set to 0, 1, or 2!\n");
	if (fapprox < 0 || fapprox > 15)
		env->ThrowError("nnedi3_rpow2:  fapprox must be [0,15]!\n");
	AVSValue v = args[0].AsClip();
	try 
	{
		double hshift = 0.0, vshift = 0.0;
		if (vi.IsRGB24())
		{
			for (int i=0; i<ct; ++i)
			{
				v = new nnedi3(v.AsClip(),i==0?1:0,true,true,true,true,nsize,nns,qual,etype,pscrn,threads,opt,fapprox,env);
				v = env->Invoke("TurnRight",v).AsClip();
				v = new nnedi3(v.AsClip(),i==0?1:0,true,true,true,true,nsize,nns,qual,etype,pscrn,threads,opt,fapprox,env);
				v = env->Invoke("TurnLeft",v).AsClip();
			}
			hshift = vshift = -0.5;
		}
		else if (vi.IsYV12())
		{
			for (int i=0; i<ct; ++i)
			{
				v = new nnedi3(v.AsClip(),i==0?1:0,true,true,true,true,nsize,nns,qual,etype,pscrn,threads,opt,fapprox,env);
				v = env->Invoke("TurnRight",v).AsClip();
				// always use field=1 to keep chroma/luma horizontal alignment
				v = new nnedi3(v.AsClip(),1,true,true,true,true,nsize,nns,qual,etype,pscrn,threads,opt,fapprox,env);
				v = env->Invoke("TurnLeft",v).AsClip();
			}
			// Correct chroma shift (it's always 1/2 pixel upwards).
			// Need a cache here because v/vc will both request from this point.
			v = env->Invoke("InternalCache",v).AsClip();
			v.AsClip()->SetCacheHints(CACHE_RANGE,2);
			AVSValue sargs[7] = { v, vi.width*rfactor, vi.height*rfactor, 0.0, -0.5, 
				vi.width*rfactor, vi.height*rfactor };
			const char *nargs[7] = { 0, 0, 0, "src_left", "src_top", 
				"src_width", "src_height" };
			AVSValue vc = env->Invoke("Spline36Resize",AVSValue(sargs,7),nargs).AsClip();
			AVSValue margs[2] = { v, vc };
			v = env->Invoke("MergeChroma",AVSValue(margs,2)).AsClip();
			for (int i=0; i<ct; ++i)
				hshift = hshift*2.0-0.5;
			vshift = -0.5;
		}
		else
		{
			// Unfortunately, turnleft()/turnright() can't preserve YUY2 chroma, so we convert
			// U/V planes to Y planes in separate clips and process them that way.
			AVSValue vu = env->Invoke("UtoY",v).AsClip();
			AVSValue vv = env->Invoke("VtoY",v).AsClip();
			for (int i=0; i<ct; ++i)
			{
				v = new nnedi3(v.AsClip(),i==0?1:0,true,true,false,false,nsize,nns,qual,etype,pscrn,threads,opt,fapprox,env);
				v = env->Invoke("TurnRight",v).AsClip();
				// always use field=1 to keep chroma/luma horizontal alignment
				v = new nnedi3(v.AsClip(),1,true,true,false,false,nsize,nns,qual,etype,pscrn,threads,opt,fapprox,env);
				v = env->Invoke("TurnLeft",v).AsClip();
			}
			for (int i=0; i<ct; ++i)
			{
				vu = new nnedi3(vu.AsClip(),i==0?1:0,true,true,false,false,nsize,nns,qual,etype,pscrn,threads,opt,fapprox,env);
				vu = env->Invoke("TurnRight",vu).AsClip();
				// always use field=1 to keep chroma/luma horizontal alignment
				vu = new nnedi3(vu.AsClip(),1,true,true,false,false,nsize,nns,qual,etype,pscrn,threads,opt,fapprox,env);
				vu = env->Invoke("TurnLeft",vu).AsClip();
			}
			for (int i=0; i<ct; ++i)
			{
				vv = new nnedi3(vv.AsClip(),i==0?1:0,true,true,false,false,nsize,nns,qual,etype,pscrn,threads,opt,fapprox,env);
				vv = env->Invoke("TurnRight",vv).AsClip();
				// always use field=1 to keep chroma/luma horizontal alignment
				vv = new nnedi3(vv.AsClip(),1,true,true,false,false,nsize,nns,qual,etype,pscrn,threads,opt,fapprox,env);
				vv = env->Invoke("TurnLeft",vv).AsClip();
			}
			AVSValue ytouvargs[3] = { vu, vv, v };
			v = env->Invoke("YtoUV",AVSValue(ytouvargs,3)).AsClip();
			for (int i=0; i<ct; ++i)
				hshift = hshift*2.0-0.5;
			vshift = -0.5;
		}
		if (cshift[0])
		{
			int type = 0;
			if (_strnicmp(cshift,"blackmanresize",14) == 0 ||
				_strnicmp(cshift,"lanczosresize",13) == 0 ||
				_strnicmp(cshift,"sincresize",10) == 0)
				type = 1;
			else if (_strnicmp(cshift,"gaussresize",11) == 0)
				type = 2;
			else if (_strnicmp(cshift,"bicubicresize",13) == 0)
				type = 3;
			if (!type || (type != 3 && ep0 == -FLT_MAX) ||
				(type == 3 && ep0 == -FLT_MAX && ep1 == -FLT_MAX))
			{
				AVSValue sargs[7] = { v, fwidth, fheight, hshift, vshift, 
					vi.width*rfactor, vi.height*rfactor };
				const char *nargs[7] = { 0, 0, 0, "src_left", "src_top", 
					"src_width", "src_height" };
				v = env->Invoke(cshift,AVSValue(sargs,7),nargs).AsClip();
			}
			else if (type != 3 || min(ep0,ep1) == -FLT_MAX)
			{
				AVSValue sargs[8] = { v, fwidth, fheight, hshift, vshift, 
					vi.width*rfactor, vi.height*rfactor, type==1?AVSValue((int)(ep0+0.5f)):
					(type==2?ep0:max(ep0,ep1)) };
				const char *nargs[8] = { 0, 0, 0, "src_left", "src_top", 
					"src_width", "src_height", type==1?"taps":(type==2?"p":(max(ep0,ep1)==ep0?"b":"c")) };
				v = env->Invoke(cshift,AVSValue(sargs,8),nargs).AsClip();
			}
			else
			{
				AVSValue sargs[9] = { v, fwidth, fheight, hshift, vshift, 
					vi.width*rfactor, vi.height*rfactor, ep0, ep1 };
				const char *nargs[9] = { 0, 0, 0, "src_left", "src_top", 
					"src_width", "src_height", "b", "c" };
				v = env->Invoke(cshift,AVSValue(sargs,9),nargs).AsClip();
			}
		}
	}
	catch (IScriptEnvironment::NotFound)
	{
		env->ThrowError("nnedi3_rpow2:  error using env->invoke (function not found)!\n");
	}
	return v;
}

extern "C" __declspec(dllexport) const char* __stdcall AvisynthPluginInit2(IScriptEnvironment* env) 
{
    env->AddFunction("nnedi3", "c[field]i[dh]b[Y]b[U]b[V]b[nsize]i[nns]i[qual]i[etype]i[pscrn]i" \
		"[threads]i[opt]i[fapprox]i", Create_nnedi3, 0);
	env->AddFunction("nnedi3_rpow2", "c[rfactor]i[nsize]i[nns]i[qual]i[etype]i[pscrn]i[cshift]s[fwidth]i" \
		"[fheight]i[ep0]f[ep1]f[threads]i[opt]i[fapprox]i", Create_nnedi3_rpow2, 0);
	return 0;
}


// new prescreener functions

void uc2s64_C(const unsigned char *t, const int pitch, float *p)
{
	short *ps = (short*)p;
	for (int y=0; y<4; ++y)
		for (int x=0; x<16; ++x)
			ps[y*16+x] = t[y*pitch*2+x];
}

__declspec(naked) void uc2s64_SSE2(const unsigned char *t, const int pitch, float *p)
{
	__asm
	{
		mov eax,[esp+4]
		mov ecx,[esp+8]
		mov edx,[esp+12]
		pxor xmm7,xmm7
		movq xmm0,QWORD PTR[eax]
		movq xmm1,QWORD PTR[eax+8]
		movq xmm2,QWORD PTR[eax+ecx*2]
		movq xmm3,QWORD PTR[eax+ecx*2+8]
		punpcklbw xmm0,xmm7
		punpcklbw xmm1,xmm7
		punpcklbw xmm2,xmm7
		punpcklbw xmm3,xmm7
		lea eax,[eax+ecx*4]
		movdqa [edx],xmm0
		movdqa [edx+16],xmm1
		movdqa [edx+32],xmm2
		movdqa [edx+48],xmm3
		movq xmm4,QWORD PTR[eax]
		movq xmm5,QWORD PTR[eax+8]
		movq xmm6,QWORD PTR[eax+ecx*2]
		movq xmm0,QWORD PTR[eax+ecx*2+8]
		punpcklbw xmm4,xmm7
		punpcklbw xmm5,xmm7
		punpcklbw xmm6,xmm7
		punpcklbw xmm0,xmm7
		movdqa [edx+64],xmm4
		movdqa [edx+80],xmm5
		movdqa [edx+96],xmm6
		movdqa [edx+112],xmm0
		ret
	}
}

void computeNetwork0new_C(const float *datai, const float *weights, unsigned char *d)
{
	short *data = (short*)datai;
	short *ws = (short*)weights;
	float *wf = (float*)&ws[4*64];
	float vals[8];
	for (int i=0; i<4; ++i)
	{
		int sum = 0;
		for (int j=0; j<64; ++j)
			sum += data[j]*ws[(i<<3)+((j>>3)<<5)+(j&7)];
		const float t = sum*wf[i]+wf[4+i];
		vals[i] = t/(1.0f+fabsf(t));
	}
	for (int i=0; i<4; ++i)
	{
		float sum = 0.0f;
		for (int j=0; j<4; ++j)
			sum += vals[j]*wf[8+i+(j<<2)];
		vals[4+i] = sum+wf[8+16+i];
	}
	int mask = 0;
	for (int i=0; i<4; ++i)
	{
		if (vals[4+i]>0.0f)
			mask |= (0x1<<(i<<3));
	}
	((int*)d)[0] = mask;
}

__declspec(naked) void computeNetwork0new_SSE2(const float *datai, const float *weights,
	unsigned char *d)
{
	__asm 
	{
		mov ecx,[esp+4]		//data
		mov eax,[esp+8]		//weights
		movdqa xmm0,[ecx]
		movdqa xmm1,xmm0
		movdqa xmm2,xmm0
		movdqa xmm3,xmm0
		pmaddwd xmm0,[eax]
		pmaddwd xmm1,[eax+16]
		pmaddwd xmm2,[eax+32]
		pmaddwd xmm3,[eax+48]
		movdqa xmm4,[ecx+16]
		movdqa xmm5,xmm4
		movdqa xmm6,xmm4
		movdqa xmm7,xmm4
		pmaddwd xmm4,[eax+64]
		pmaddwd xmm5,[eax+80]
		pmaddwd xmm6,[eax+96]
		pmaddwd xmm7,[eax+112]
		paddd xmm0,xmm4
		paddd xmm1,xmm5
		paddd xmm2,xmm6
		paddd xmm3,xmm7
		movdqa xmm4,[ecx+32]
		movdqa xmm5,xmm4
		movdqa xmm6,xmm4
		movdqa xmm7,xmm4
		pmaddwd xmm4,[eax+128]
		pmaddwd xmm5,[eax+144]
		pmaddwd xmm6,[eax+160]
		pmaddwd xmm7,[eax+176]
		paddd xmm0,xmm4
		paddd xmm1,xmm5
		paddd xmm2,xmm6
		paddd xmm3,xmm7
		movdqa xmm4,[ecx+48]
		movdqa xmm5,xmm4
		movdqa xmm6,xmm4
		movdqa xmm7,xmm4
		pmaddwd xmm4,[eax+192]
		pmaddwd xmm5,[eax+208]
		pmaddwd xmm6,[eax+224]
		pmaddwd xmm7,[eax+240]
		paddd xmm0,xmm4
		paddd xmm1,xmm5
		paddd xmm2,xmm6
		paddd xmm3,xmm7
		movdqa xmm4,[ecx+64]
		movdqa xmm5,xmm4
		movdqa xmm6,xmm4
		movdqa xmm7,xmm4
		pmaddwd xmm4,[eax+256]
		pmaddwd xmm5,[eax+272]
		pmaddwd xmm6,[eax+288]
		pmaddwd xmm7,[eax+304]
		paddd xmm0,xmm4
		paddd xmm1,xmm5
		paddd xmm2,xmm6
		paddd xmm3,xmm7
		movdqa xmm4,[ecx+80]
		movdqa xmm5,xmm4
		movdqa xmm6,xmm4
		movdqa xmm7,xmm4
		pmaddwd xmm4,[eax+320]
		pmaddwd xmm5,[eax+336]
		pmaddwd xmm6,[eax+352]
		pmaddwd xmm7,[eax+368]
		paddd xmm0,xmm4
		paddd xmm1,xmm5
		paddd xmm2,xmm6
		paddd xmm3,xmm7
		movdqa xmm4,[ecx+96]
		movdqa xmm5,xmm4
		movdqa xmm6,xmm4
		movdqa xmm7,xmm4
		pmaddwd xmm4,[eax+384]
		pmaddwd xmm5,[eax+400]
		pmaddwd xmm6,[eax+416]
		pmaddwd xmm7,[eax+432]
		paddd xmm0,xmm4
		paddd xmm1,xmm5
		paddd xmm2,xmm6
		paddd xmm3,xmm7
		movdqa xmm4,[ecx+112]
		movdqa xmm5,xmm4
		movdqa xmm6,xmm4
		movdqa xmm7,xmm4
		pmaddwd xmm4,[eax+448]
		pmaddwd xmm5,[eax+464]
		pmaddwd xmm6,[eax+480]
		pmaddwd xmm7,[eax+496]
		paddd xmm0,xmm4
		paddd xmm1,xmm5
		paddd xmm2,xmm6
		paddd xmm3,xmm7
		movdqa xmm4,xmm0
		movdqa xmm5,xmm2
		punpcklqdq xmm0,xmm1
		punpcklqdq xmm2,xmm3
		punpckhqdq xmm4,xmm1
		punpckhqdq xmm5,xmm3
		paddd xmm0,xmm4
		paddd xmm2,xmm5
		movdqa xmm6,xmm0
		shufps xmm0,xmm2,136
		shufps xmm6,xmm2,221
		paddd xmm0,xmm6
		cvtdq2ps xmm0,xmm0
		mulps xmm0,[eax+512]
		addps xmm0,[eax+528]
		movaps xmm1,xmm0
		andps xmm0,sign_bits_f
		addps xmm0,ones_f
		rcpps xmm0,xmm0
		mulps xmm0,xmm1
		pshufd xmm1,xmm0,0
		pshufd xmm2,xmm0,85
		pshufd xmm3,xmm0,170
		pshufd xmm4,xmm0,255
		mulps xmm1,[eax+544]
		mulps xmm2,[eax+560]
		mulps xmm3,[eax+576]
		mulps xmm4,[eax+592]
		pxor xmm0,xmm0
		addps xmm1,xmm2
		addps xmm3,xmm4
		addps xmm1,xmm3
		mov ecx,[esp+12]
		addps xmm1,[eax+608]
		cmpps xmm1,xmm0,1
		packssdw xmm1,xmm0
		packsswb xmm1,xmm0
		movd eax,xmm1
		xor eax,0xFFFFFFFF
		and eax,0x01010101
		mov [ecx],eax
		ret
	}
}