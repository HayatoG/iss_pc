// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "imgui.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
namespace ShaderLoading {
inline void Draw(uint32_t done,uint32_t total,bool compiling,double elapsed,bool pt,bool first_light,ImFont*title_font=nullptr,ImTextureData*background=nullptr){
 auto*v=ImGui::GetMainViewport();auto*d=ImGui::GetForegroundDrawList();float width=v->WorkSize.x,height=v->WorkSize.y;if(width<=0||height<=0)return;
 float k=std::min(width/1280.f,height/800.f),w=width/k,h=height/k;
 auto p=[&](float x,float y){return ImVec2(v->WorkPos.x+x*k,v->WorkPos.y+y*k);};
 auto font=[&](float s){return s>=30&&title_font?title_font:ImGui::GetFont();};
 auto measure=[&](const char*s,float size){return font(size)->CalcTextSizeA(size*k,1.e8f,0,s).x/k;};
 auto text=[&](float x,float y,float size,ImU32 c,const char*s,float wrap=0.f){d->AddText(font(size),size*k,p(x,y),c,s,nullptr,wrap*k);};
 ImU32 white=IM_COL32(238,238,225,255),muted=IM_COL32(172,179,165,255),quiet=IM_COL32(112,123,109,255),accent=IM_COL32(199,208,183,255);
 d->PushClipRect(v->WorkPos,p(w,h),true);
if(background&&background->Width>0&&background->Height>0){
 ImVec2 uv0(0,0),uv1(1,1);float image_aspect=float(background->Width)/background->Height,view_aspect=width/height;
 if(image_aspect>view_aspect){float span=view_aspect/image_aspect;uv0.x=(1-span)*.5f;uv1.x=1-uv0.x;}
 else{float span=image_aspect/view_aspect;uv0.y=(1-span)*.5f;uv1.y=1-uv0.y;}
 d->AddImage(background->GetTexRef(),v->WorkPos,p(w,h),uv0,uv1);
 d->AddRectFilledMultiColor(v->WorkPos,p(w,h),IM_COL32(15,18,14,55),IM_COL32(10,13,10,232),IM_COL32(8,10,8,245),IM_COL32(12,15,11,150));
}else d->AddRectFilledMultiColor(v->WorkPos,p(w,h),IM_COL32(34,36,31,255),IM_COL32(18,21,17,255),IM_COL32(8,10,8,255),IM_COL32(19,23,18,255));
 d->AddLine(p(56,69),p(w-56,69),IM_COL32(189,200,177,90),k);
 text(56,32,16,white,"inFAMOUS / PC");text(w-56-measure("GRAPHICS INITIALIZATION",12),34,12,muted,"GRAPHICS INITIALIZATION");text(56,89,12,quiet,first_light?"FIRST LIGHT":"SECOND SON");
 float x=w*.51f,y=std::max(164.f,h*.30f),body=w-x-72;bool known=total>0;done=known?std::min(done,total):0;bool ready=known&&done==total;
 text(x,y-30,12,accent,pt?"PREPARAÇÃO DO SISTEMA":"SYSTEM PREPARATION");
 text(x,y,34,white,ready?(pt?"Iniciando o jogo":"Starting your game"):compiling?(pt?"Compilando shaders":"Compiling shaders"):known?(pt?"Carregando shaders":"Loading shader cache"):(pt?"Preparando os gráficos":"Preparing graphics"));
 text(x,y+69,18,muted,pt?"A compilação de shaders sempre demora mais na primeira execução após uma atualização...":"Compiling Shaders always takes the longest on the first run after updating...",body);
 text(x,y+180,12,quiet,pt?"AGUARDE. O JOGO INICIARÁ AUTOMATICAMENTE.":"PLEASE WAIT. YOUR GAME WILL START AUTOMATICALLY.",body);
 float by=h-128,bw=w-112;text(56,by-38,12,accent,compiling?"SHADER COMPILATION":known?"SHADER CACHE":"GRAPHICS PREPARATION");
 char percent[24];if(known)std::snprintf(percent,sizeof(percent),"%u%%",uint32_t(uint64_t(done)*100/total));else std::snprintf(percent,sizeof(percent),"--%%");text(w-56-measure(percent,42),by-62,42,white,percent);
 d->AddRectFilled(p(56,by),p(w-56,by+6),IM_COL32(104,114,98,95));if(known&&done)d->AddRectFilled(p(56,by),p(56+bw*float(done)/total,by+6),accent);
 else if(!known){float t=float(std::fmod(ImGui::GetTime()*.2,1.0)),l=std::max(0.f,t*1.2f-.2f),r=std::min(1.f,t*1.2f);if(r>l)d->AddRectFilled(p(56+bw*l,by),p(56+bw*r,by+6),accent);}
 d->AddLine(p(56,by+8),p(w-56,by+8),IM_COL32(210,217,198,160),k);
 char count[96];if(known)std::snprintf(count,sizeof(count),"%u / %u shaders",done,total);else std::snprintf(count,sizeof(count),"%s",pt?"Aguardando o total de shaders":"Waiting for shader totals");text(56,by+20,14,muted,count);
 const char*status=ready?(pt?"Etapa concluída":"Stage complete"):(pt?"Preparando a próxima etapa":"Preparing the next stage");char estimate[96];
 if(compiling&&known&&done>=5&&done<total&&elapsed>=3&&std::isfinite(elapsed)){uint32_t left=uint32_t(std::min(86400.,elapsed*double(total-done)/double(done)))+1;std::snprintf(estimate,sizeof(estimate),pt?"Tempo estimado restante  %u:%02u":"Estimated time remaining  %u:%02u",left/60,left%60);status=estimate;}
 text(w-56-measure(status,14),by+20,14,muted,status);text(56,h-48,13,quiet,pt?"Pressione F1 no jogo para abrir o Overlay ou R3+L2/LT no controle":"Press F1 in-game to get the Overlay to show up or press R3+L2/LT on controller",bw);d->PopClipRect();
}
}
