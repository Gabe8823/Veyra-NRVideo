#pragma once
#include "veyra/pipeline/HdrToneMap.h"
#include <limits>
namespace hdrToneTests {
// Independent BT.2390 reference: cubic Bezier evaluated by de Casteljau,
// rather than the shader's expanded Hermite polynomial. ST2084 utilities
// belong to the separately verified native HDR round-trip reference.
inline double luminance(double y,double peak){
    if(y<=0)return 0;if(peak<=203)return std::min(y/203,1.0);
    const double black=hdrRoundTrip::pq(0),span=hdrRoundTrip::pq(peak)-black;
    const double top=(hdrRoundTrip::pq(203)-black)/span,k=1.5*top-.5;
    double x=std::clamp((hdrRoundTrip::pq(y)-black)/span,0.0,1.0);
    if(x>=k){
        const double t=(x-k)/(1-k);
        std::array<double,4> p={k,k+(1-k)/3,top,top};
        for(unsigned n=3;n>0;--n)for(unsigned i=0;i<n;++i)p[i]=std::lerp(p[i],p[i+1],t);
        x=p[0];
    }
    return std::clamp(hdrRoundTrip::nits(x*span+black)/203,0.0,1.0);
}
inline int run(veyra::gfx::D3D12DeviceContext& ctx,veyra::gfx::CommandSlotRing& ring){
    using namespace veyra;int failures=0;
    pipeline::ColorDescription policy;policy.transfer=pipeline::TransferFunction::PQ;
    bool metadata=pipeline::hdrToneMapPeak(policy).nits==1000;
    policy.hdrMasteringPeakNits=4000;metadata&=pipeline::hdrToneMapPeak(policy).nits==4000;
    policy.hdrMaxCllNits=600;metadata&=pipeline::hdrToneMapPeak(policy).nits==600;
    policy.hdrMaxFallNits=700;metadata&=pipeline::hdrToneMapPeak(policy).nits==4000;
    policy.hdrMaxCllNits=std::numeric_limits<float>::quiet_NaN();metadata&=pipeline::hdrToneMapPeak(policy).nits==4000;
    policy.hdrMasteringPeakNits=20000;metadata&=pipeline::hdrToneMapPeak(policy).nits==1000;
    policy.hdrMaxCllNits=50;policy.hdrMaxFallNits=0;metadata&=pipeline::hdrToneMapPeak(policy).nits==203;
    policy.transfer=pipeline::TransferFunction::HLG;metadata&=pipeline::hdrToneMapPeak(policy).nits==1000;
    AVFrame* metadataFrame=av_frame_alloc();auto* cll=av_content_light_metadata_create_side_data(metadataFrame);
    if(!cll){av_frame_free(&metadataFrame);return 1;}
    policy.hdrMaxCllNits=600;policy.hdrMaxFallNits=100;
    metadata&=pipeline::resolveFrameColor(*metadataFrame,policy).hdrMaxCllNits==600;
    cll->MaxCLL=4000;cll->MaxFALL=300;
    metadata&=pipeline::resolveFrameColor(*metadataFrame,policy).hdrMaxCllNits==4000;
    cll->MaxFALL=5000;metadata&=pipeline::resolveFrameColor(*metadataFrame,policy).hdrMaxCllNits==600;
    av_frame_free(&metadataFrame);
    std::cout<<"HDR_TONE_METADATA pass="<<metadata<<std::endl;failures+=!metadata;
    for(bool hlg:{false,true})for(double declared:{203.,400.,1000.,4000.,10000.})for(bool full:{false,true}){
        constexpr unsigned count=128,width=count*4,height=16;const double peak=hlg?1000:declared;
        AVFrame* f=av_frame_alloc();f->format=AV_PIX_FMT_YUV420P10LE;f->width=width;f->height=height;
        f->color_range=full?AVCOL_RANGE_JPEG:AVCOL_RANGE_MPEG;f->colorspace=AVCOL_SPC_BT2020_NCL;f->color_primaries=AVCOL_PRI_BT2020;f->color_trc=hlg?AVCOL_TRC_ARIB_STD_B67:AVCOL_TRC_SMPTE2084;
        if(av_frame_get_buffer(f,32)<0)return failures+1;
        std::array<std::array<double,3>,count> linear{};
        for(unsigned i=0;i<count;++i){
            const double level=i==0?0:std::pow(10.,-3.+7.*std::min(i,103u)/103.);
            std::array<double,3> n={level,level,level};
            if(i>=104){const unsigned j=i-104;n={double((j*37)%100+1)*10,double((j*61)%100+1)*10,double((j*17)%100+1)*10};if(j<3){n={0,0,0};n[j]=1000;}}
            std::array<double,3> signal;for(unsigned c=0;c<3;++c)signal[c]=hlg?hdrRoundTrip::hlgEncode(n[c]/10000):hdrRoundTrip::pq(n[c]);
            const double y=.2627*signal[0]+.6780*signal[1]+.0593*signal[2];
            const int yy=int(std::lround((full?0:64)+(full?1023:876)*y)),u=int(std::lround(512+(full?1023:896)*(signal[2]-y)/1.8814)),v=int(std::lround(512+(full?1023:896)*(signal[0]-y)/1.4746));
            for(unsigned row=0;row<height;++row)for(unsigned x=i*4;x<i*4+4;++x){reinterpret_cast<uint16_t*>(f->data[0]+row*f->linesize[0])[x]=uint16_t(yy);
                if(!(row%2)&&!(x%2)){reinterpret_cast<uint16_t*>(f->data[1]+row/2*f->linesize[1])[x/2]=uint16_t(u);reinterpret_cast<uint16_t*>(f->data[2]+row/2*f->linesize[2])[x/2]=uint16_t(v);}}
            const double l=(yy-(full?0.:64.))/(full?1023:876),cb=(u-512.)/(full?1023:896),cr=(v-512.)/(full?1023:896);
            signal={l+1.4746*cr,l-.1645531268436578*cb-.5713531268436578*cr,l+1.8814*cb};
            if(hlg){for(auto& c:signal)c=hdrRoundTrip::hlgDecode(c);double gain=1000*std::pow(.2627*signal[0]+.678*signal[1]+.0593*signal[2],.2);for(auto& c:signal)c*=gain;}
            else for(auto& c:signal)c=hdrRoundTrip::nits(c);
            linear[i]={1.660491*signal[0]-.587641*signal[1]-.072850*signal[2],-.124550*signal[0]+1.132900*signal[1]-.008349*signal[2],-.018151*signal[0]-.100579*signal[1]+1.118730*signal[2]};
        }
        pipeline::EnhanceGraph graph(ctx,ring);pipeline::EnhanceGraphDesc gd;gd.sourceWidth=gd.workWidth=width;gd.sourceHeight=gd.workHeight=height;gd.hdrInput=true;gd.enableNr=gd.enableSr=gd.enableFg=false;gd.noFeatures=true;
        pipeline::ColorDescription color;color.hdrMaxCllNits=float(declared);
        pipeline::EnhanceGraph::FrameOutputs out;std::vector<float> actual;
        bool ok=graph.initialize(gd)&&graph.createViews()&&graph.process(f,0,true,out,1,&color)&&captureReadFp16(ctx,ring,graph.diagnosticLinearInput(),actual);
        double yError=0,hueError=0,last=-1;bool bounded=true,monotone=true,neutral=true;
        if(ok)for(unsigned i=0;i<count;++i){
            const auto* p=actual.data()+(i*4+2)*4;const auto& c=linear[i];const double sourceY=.212639*c[0]+.715169*c[1]+.072192*c[2];
            const double expected=luminance(sourceY,peak),y=.212639*p[0]+.715169*p[1]+.072192*p[2];yError=std::max(yError,std::abs(y-expected));
            for(unsigned k=0;k<3;++k)bounded&=std::isfinite(p[k])&&p[k]>=-1e-5&&p[k]<=1.0001;
            if(i<104){monotone&=y+1e-5>=last;last=y;neutral&=std::max({p[0],p[1],p[2]})-std::min({p[0],p[1],p[2]})<.001;}
            else if(sourceY>1e-6&&expected>.001&&expected<.99){
                // Common chroma scaling must preserve the linear RGB hue ray.
                std::array<double,3> a={c[0]/sourceY-1,c[1]/sourceY-1,c[2]/sourceY-1},b={p[0]-y,p[1]-y,p[2]-y};
                const double norm=std::sqrt(a[0]*a[0]+a[1]*a[1]+a[2]*a[2]);
                if(norm>.001)for(unsigned k=0;k<3;++k)hueError=std::max(hueError,std::abs(a[k]*b[(k+1)%3]-a[(k+1)%3]*b[k])/norm);
            }
        }
        ok=ok&&bounded&&monotone&&neutral&&yError<.0015&&hueError<.001;
        std::cout<<"HDR_TONE_GPU hlg="<<hlg<<" declaredPeak="<<declared<<" full="<<full<<" luminanceError="<<yError<<" hueRayError="<<hueError<<" bounded="<<bounded<<" monotone="<<monotone<<" neutral="<<neutral<<" pass="<<ok<<std::endl;
        if(ok&&!hlg&&!full&&declared==1000){
            // Static metadata must not pump the picture across seeks/cuts in
            // the same graph. A new source gets a fresh graph and selection.
            out={};color.hdrMaxCllNits=4000;std::vector<float> second;
            const bool stable=graph.process(f,33.333,true,out,2,&color)&&captureReadFp16(ctx,ring,graph.diagnosticLinearInput(),second)&&actual==second;
            std::cout<<"HDR_TONE_STATIC_RESET pass="<<stable<<std::endl;ok&=stable;
        }
        failures+=!ok;out={};ring.drainQueue();graph.shutdown();av_frame_free(&f);
    }
    return failures;
}
}
