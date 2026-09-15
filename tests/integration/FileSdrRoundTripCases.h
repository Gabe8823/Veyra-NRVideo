#pragma once
#include "veyra/source/MediaFileSource.h"
extern "C" {
#include <libavutil/hwcontext.h>
}

// Diagnostic readbacks only. Expected RGB comes directly from decoded YUV
// and legal-range/matrix equations, not from the graph's transfer functions.
inline int fileSdrRoundTripCases(veyra::gfx::D3D12DeviceContext& ctx,
    veyra::gfx::CommandSlotRing& ring,const wchar_t* path) {
    using namespace veyra;int failures=0;
    for(bool hardware:{false,true}) {
        source::MediaFileSource source;source::SourceOpenDesc open;open.path=path;
        open.preferHardwareDecode=hardware;open.d3d12Device=ctx.device();open.d3d12Queue=ctx.directQueue();
        if(!source.open(open)||!source.seek({60,1}))return failures+1;
        pipeline::FramePacket packet;const AVFrame* decoded=nullptr;
        if(source.read(packet,&decoded)!=source::SourceReadStatus::Frame||!decoded)return failures+1;
        AVFrame* transferred=av_frame_alloc();const AVFrame* reference=decoded;
        if(decoded->hw_frames_ctx) {
            if(av_hwframe_transfer_data(transferred,decoded,0)<0){av_frame_free(&transferred);return failures+1;}
            reference=transferred;
        }
        const bool nv12=reference->format==AV_PIX_FMT_NV12;
        if((!nv12&&reference->format!=AV_PIX_FMT_YUV420P)||packet.colorInfo.isHdrPath()||
            packet.colorInfo.matrix!=pipeline::YuvMatrix::BT709){av_frame_free(&transferred);return failures+1;}
        pipeline::EnhanceGraph graph(ctx,ring);pipeline::EnhanceGraphDesc gd;
        gd.sourceWidth=gd.workWidth=decoded->width;gd.sourceHeight=gd.workHeight=decoded->height;
        gd.enableNr=gd.enableSr=gd.enableFg=false;gd.noFeatures=true;
        pipeline::EnhanceGraph::FrameOutputs output;sink::RgbaImage image;
        bool ok=graph.initialize(gd)&&graph.createViews()&&graph.process(decoded,60000,true,output,1,&packet.colorInfo)&&
            sink::readRgba8(ctx,ring,graph.videoFrameResource(output.videoSlot),image);
        double maximum=0,sum=0,bias=0;size_t samples=0;
        const bool full=packet.colorInfo.range==pipeline::ColorRange::Full;
        if(ok)for(unsigned y=0;y<image.height;++y)for(unsigned x=0;x<image.width;++x) {
            const double yy=std::clamp((reference->data[0][size_t(y)*reference->linesize[0]+x]-(full?0.0:16.0))/(full?255.0:219.0),0.0,1.0);
            const size_t offset=size_t(y/2)*reference->linesize[1]+(nv12?x/2*2:x/2);
            const double u=(reference->data[1][offset]-128.0)/(full?255.0:224.0);
            const double v=((nv12?reference->data[1][offset+1]:reference->data[2][size_t(y/2)*reference->linesize[2]+x/2])-128.0)/(full?255.0:224.0);
            const double rgb[]={yy+1.5748*v,yy-.1873242729306488*u-.4681242729306488*v,yy+1.8556*u};
            for(unsigned c=0;c<3;++c){double error=image.pixels[(size_t(y)*image.width+x)*4+c]-255*std::clamp(rgb[c],0.0,1.0);maximum=std::max(maximum,std::abs(error));sum+=std::abs(error);bias+=error;++samples;}
        }
        ok=ok&&samples&&maximum<2&&sum/samples<.6&&(!hardware||source.info().hardwareDecodeActive);
        std::cout<<"FILE_SDR_ROUNDTRIP hardware="<<hardware<<" active="<<source.info().hardwareDecodeActive
            <<" pts="<<decoded->pts<<" maxCodeError="<<maximum<<" meanCodeError="<<(samples?sum/samples:-1)
            <<" signedBias="<<(samples?bias/samples:-1)<<" pass="<<ok<<std::endl;
        failures+=!ok;
        if(!image.pixels.empty()) {
            HWND window=CreateWindowExW(0,L"STATIC",L"SDR file diagnostic",WS_POPUP,0,0,image.width,image.height,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
            engine::VideoPresenter presenter;sink::RgbaImage shown;
            bool presented=window&&presenter.open(ctx,window,graph)&&presenter.present(ctx,ring,graph,output.videoSlot,false)&&presenter.readPresentedFrameForTest(ctx,ring,shown);
            unsigned maxPresent=0;
            if(presented&&shown.pixels.size()==image.pixels.size())for(size_t i=0;i<shown.pixels.size();++i)maxPresent=std::max(maxPresent,unsigned(std::abs(int(shown.pixels[i])-int(image.pixels[i]))));
            else presented=false;
            presented=presented&&maxPresent<=1;
            std::cout<<"FILE_SDR_PRESENT hardware="<<hardware<<" maxCodeError="<<maxPresent<<" pass="<<presented<<std::endl;
            failures+=!presented;ring.drainQueue();presenter.close();if(window)DestroyWindow(window);
        }
        output={};ring.drainQueue();graph.shutdown();av_frame_free(&transferred);source.close();
    }
    return failures;
}
