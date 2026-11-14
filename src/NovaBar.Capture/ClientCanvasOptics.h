#pragma once
#include "PanelGlassPixels.h"
#include <cstring>

namespace novabar
{
    struct ClientOpticsStats { size_t EdgePixels = 0; };

    inline bool RenderClientCanvasOptics(PixelFrame const& immutableBase, PixelRect target,
        double radius, double lens, double dispersion, double specular, PixelRect viewport,
        std::vector<uint8_t>& out, ClientOpticsStats* stats = nullptr)
    {
        if (stats) stats->EdgePixels = 0;
        const int width=immutableBase.Width,height=immutableBase.Height;
        const auto canvas=immutableBase.Bounds;
        const int64_t cw=static_cast<int64_t>(canvas.Right)-canvas.Left;
        const int64_t ch=static_cast<int64_t>(canvas.Bottom)-canvas.Top;
        const int64_t tw=static_cast<int64_t>(target.Right)-target.Left;
        const int64_t th=static_cast<int64_t>(target.Bottom)-target.Top;
        if (&out==&immutableBase.Pixels || width<=0 || height<=0 || width>16384 || height>16384
            || static_cast<uint64_t>(width)*height>8ull*1024*1024
            || immutableBase.Pixels.size()!=static_cast<uint64_t>(width)*height*4
            || cw<=0 || ch<=0 || cw>32768 || ch>32768 || tw<=0 || th<=0 || tw>32768 || th>32768
            || viewport.Right<=viewport.Left || viewport.Bottom<=viewport.Top
            || !std::isfinite(radius) || radius<0 || !std::isfinite(lens)
            || !std::isfinite(dispersion) || !std::isfinite(specular)) return false;
        out.resize(immutableBase.Pixels.size());
        std::memcpy(out.data(),immutableBase.Pixels.data(),out.size());
        lens=panel_detail::Unit(lens); dispersion=panel_detail::Unit(dispersion)*.18;
        const float lightStrength=static_cast<float>(panel_detail::Unit(specular));
        if (!lens && !lightStrength) return true;
        const auto geometry=panel_detail::MakeEdgeGeometry(target,radius);
        const double sx=static_cast<double>(width)/cw,sy=static_cast<double>(height)/ch;
        const double pw=1/sx,ph=1/sy;
        const panel_detail::Sampler sampler{immutableBase.Pixels.data(),width,height,
            static_cast<double>(canvas.Left),static_cast<double>(canvas.Top),sx,sy};
        const auto index=[](double value,int maximum)
        { return static_cast<int>(std::clamp(std::ceil(value-.5),0.0,static_cast<double>(maximum))); };
        const int x0=index((static_cast<double>(std::max({target.Left,canvas.Left,viewport.Left}))-canvas.Left)*sx,width);
        const int x1=index((static_cast<double>(std::min({target.Right,canvas.Right,viewport.Right}))-canvas.Left)*sx,width);
        const int y0=index((static_cast<double>(std::max({target.Top,canvas.Top,viewport.Top}))-canvas.Top)*sy,height);
        const int y1=index((static_cast<double>(std::min({target.Bottom,canvas.Bottom,viewport.Bottom}))-canvas.Top)*sy,height);
        if(x1<=x0 || y1<=y0) return true;
        const auto firstCenter=[](double origin,double edge,double scale,int maximum)
        { return origin+(std::clamp(std::ceil((edge-origin)*scale-.5),0.0,static_cast<double>(maximum-1))+.5)/scale; };
        const auto lastCenter=[](double origin,double edge,double scale,int maximum)
        { return origin+(std::clamp(std::ceil((edge-origin)*scale-.5)-1,0.0,static_cast<double>(maximum-1))+.5)/scale; };
        const double minX=firstCenter(canvas.Left,viewport.Left,sx,width),maxX=lastCenter(canvas.Left,viewport.Right,sx,width);
        const double minY=firstCenter(canvas.Top,viewport.Top,sy,height),maxY=lastCenter(canvas.Top,viewport.Bottom,sy,height);
        const auto sampleAt=[&](double x,double y,panel_detail::Sample& sample)
        {
            x=std::clamp(x,static_cast<double>(viewport.Left),static_cast<double>(viewport.Right));
            y=std::clamp(y,static_cast<double>(viewport.Top),static_cast<double>(viewport.Bottom));
            // Missing in-desktop source is an error, never a fabricated sample
            // at an arbitrary cache edge. Only true desktop edges may clamp.
            if(x<canvas.Left || x>canvas.Right || y<canvas.Top || y>canvas.Bottom) return false;
            if(canvas.Left<=viewport.Left) x=std::max(x,minX);
            if(canvas.Right>=viewport.Right) x=std::min(x,maxX);
            if(canvas.Top<=viewport.Top) y=std::max(y,minY);
            if(canvas.Bottom>=viewport.Bottom) y=std::min(y,maxY);
            sample=sampler.Read(x,y); return true;
        };
        const auto span=[&](int y,int left,int right)
        {
            const double wy=canvas.Top+(y+.5)*ph;
            for(int x=left;x<right;++x)
            {
                const double wx=canvas.Left+(x+.5)*pw;
                const auto field=panel_detail::EvaluateEdge(geometry,
                    wx-(target.Left+geometry.HalfWidth),wy-(target.Top+geometry.HalfHeight),lens,lightStrength);
                if(field.Distance>0 || field.Distance < -geometry.Band) continue;
                panel_detail::Sample sample;
                if(!sampleAt(wx+field.Dx,wy+field.Dy,sample)) return false;
                if(dispersion && field.Shift)
                {
                    panel_detail::Sample blue,red;
                    if(!sampleAt(wx+field.Dx*(1-dispersion),wy+field.Dy*(1-dispersion),blue)
                        || !sampleAt(wx+field.Dx*(1+dispersion),wy+field.Dy*(1+dispersion),red)) return false;
                    sample.B=blue.B; sample.R=red.R;
                }
                const auto at=(static_cast<size_t>(y)*width+x)*4;
                const float alpha=std::clamp(sample.A,0.0f,255.0f);
                const float highlight=field.Highlight*(alpha/255);
                out[at]=static_cast<uint8_t>(std::clamp(sample.B+highlight,0.0f,alpha)+.5f);
                out[at+1]=static_cast<uint8_t>(std::clamp(sample.G+highlight,0.0f,alpha)+.5f);
                out[at+2]=static_cast<uint8_t>(std::clamp(sample.R+highlight,0.0f,alpha)+.5f);
                out[at+3]=static_cast<uint8_t>(alpha+.5f);
                if(stats) ++stats->EdgePixels;
            }
            return true;
        };
        const int leftEnd=std::clamp(index((target.Left+geometry.Inset-canvas.Left)*sx,width),x0,x1);
        const int rightStart=std::clamp(index((target.Right-geometry.Inset-canvas.Left)*sx,width),leftEnd,x1);
        for(int y=y0;y<y1;++y)
        {
            const double localY=canvas.Top+(y+.5)*ph-(target.Top+geometry.HalfHeight);
            if(std::abs(localY)>geometry.HalfHeight-geometry.Inset)
            { if(!span(y,x0,x1)) return false; }
            else if(!span(y,x0,leftEnd) || !span(y,rightStart,x1)) return false;
        }
        return true;
    }
}
