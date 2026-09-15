#pragma once
#include "CaptureFormatGpuCases.h"
#include <array>
#include <cmath>

inline int sdrColorGpuCases(veyra::gfx::D3D12DeviceContext& ctx,veyra::gfx::CommandSlotRing& ring){
    using namespace veyra;int failures=0;
    const std::array<std::array<double,3>,6> patches={{{0,0,0},{.5,.5,.5},{1,1,1},{.7,.2,.3},{.2,.7,.3},{.2,.3,.7}}};
    for(bool wideGamut:{false,true})for(bool full:{false,true})for(int packing=0;packing<4;++packing){
        const bool rgb=packing==0,yuy2=packing==1,ten=packing==3;
        AVFrame* frame=av_frame_alloc();frame->width=48;frame->height=16;
        frame->format=rgb?AV_PIX_FMT_BGRA:yuy2?AV_PIX_FMT_YUYV422:ten?AV_PIX_FMT_YUV420P10LE:AV_PIX_FMT_YUV420P;
        if(av_frame_get_buffer(frame,32)<0){av_frame_free(&frame);return failures+1;}
        const double kr=wideGamut?.2627:.2126,kb=wideGamut?.0593:.0722,kg=1-kr-kb;
        const double max=ten?1023:255,black=full?0:ten?64:16,span=full?max:ten?876:219,chroma=full?max:ten?896:224,mid=ten?512:128;
        for(unsigned y=0;y<16;++y)for(unsigned x=0;x<48;++x){
            const auto& c=patches[x/8];const double luma=kr*c[0]+kg*c[1]+kb*c[2];
            const unsigned yy=unsigned(std::lround(black+span*luma));
            const unsigned u=unsigned(std::lround(mid+chroma*(c[2]-luma)/(2*(1-kb))));
            const unsigned v=unsigned(std::lround(mid+chroma*(c[0]-luma)/(2*(1-kr))));
            if(rgb){auto* dst=frame->data[0]+y*frame->linesize[0]+x*4;for(unsigned k=0;k<3;++k)dst[k]=uint8_t(std::lround(black+span*c[2-k]));dst[3]=255;}
            else if(yuy2){auto* dst=frame->data[0]+y*frame->linesize[0]+x*2;dst[0]=uint8_t(yy);dst[1]=uint8_t(x%2?v:u);}
            else if(ten){reinterpret_cast<uint16_t*>(frame->data[0]+y*frame->linesize[0])[x]=uint16_t(yy);if(!(y%2)&&!(x%2)){reinterpret_cast<uint16_t*>(frame->data[1]+(y/2)*frame->linesize[1])[x/2]=uint16_t(u);reinterpret_cast<uint16_t*>(frame->data[2]+(y/2)*frame->linesize[2])[x/2]=uint16_t(v);}}
            else{frame->data[0][y*frame->linesize[0]+x]=uint8_t(yy);if(!(y%2)&&!(x%2)){frame->data[1][(y/2)*frame->linesize[1]+x/2]=uint8_t(u);frame->data[2][(y/2)*frame->linesize[2]+x/2]=uint8_t(v);}}
        }
        pipeline::ColorDescription color;color.range=full?pipeline::ColorRange::Full:pipeline::ColorRange::Limited;
        color.matrix=wideGamut?pipeline::YuvMatrix::BT2020NCL:pipeline::YuvMatrix::BT709;
        color.primaries=wideGamut?pipeline::ColorPrimaries::BT2020:pipeline::ColorPrimaries::BT709;
        color.transfer=wideGamut?pipeline::TransferFunction::BT2020_10:pipeline::TransferFunction::BT709;color.preserveSdrCodeValues=true;
        pipeline::EnhanceGraph graph(ctx,ring);pipeline::EnhanceGraphDesc gd;
        gd.sourceWidth=gd.workWidth=48;gd.sourceHeight=gd.workHeight=16;gd.enableNr=gd.enableSr=gd.enableFg=false;gd.noFeatures=true;
        gd.rgbInput=rgb;gd.yuy2Input=yuy2;gd.captureBitDepth=ten?10:8;
        pipeline::EnhanceGraph::FrameOutputs out;std::vector<float> actual;
        bool ok=graph.initialize(gd)&&graph.createViews()&&graph.process(frame,0,true,out,1,&color)&&captureReadFp16(ctx,ring,graph.diagnosticLinearInput(),actual);
        double error=0;
        if(ok)for(unsigned patch=0;patch<patches.size();++patch){
            std::array<double,3> linear{};
            for(unsigned k=0;k<3;++k){const double c=patches[patch][k];linear[k]=wideGamut?(c<.081?c/4.5:std::pow((c+.099)/1.099,1/.45)):(c<=.04045?c/12.92:std::pow((c+.055)/1.055,2.4));}
            const auto reference=wideGamut?std::array<double,3>{1.660491*linear[0]-.587641*linear[1]-.072850*linear[2],-.124550*linear[0]+1.132900*linear[1]-.008349*linear[2],-.018151*linear[0]-.100579*linear[1]+1.118730*linear[2]}:linear;
            for(unsigned k=0;k<3;++k)error=std::max(error,std::abs(actual[(patch*8+4)*4+k]-reference[k]));
        }
        ok=ok&&error<(ten?.003:.015);
        std::cout<<"SDR_COLOR bt2020="<<wideGamut<<" full="<<full<<" packing="<<packing<<" linearMaxError="<<error<<" pass="<<ok<<std::endl;
        failures+=!ok;out={};ring.drainQueue();graph.shutdown();av_frame_free(&frame);
    }
    return failures;
}
