// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <cmath>
#include <cwchar>
namespace StartupLoading {
inline void Draw(Gdiplus::Graphics&g,int width,int height,int phase,unsigned done,unsigned total,double elapsed,unsigned eta_from,bool pt,bool first_light,unsigned long long ticks){
 using namespace Gdiplus;
 if(width<=0||height<=0)return;
 auto saved=g.Save();REAL k=std::min(width/1280.f,height/800.f),w=width/k,h=height/k;
 g.ScaleTransform(k,k);g.SetSmoothingMode(SmoothingModeAntiAlias);g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
 LinearGradientBrush shade(RectF(0,0,w,h),Color(55,18,22,16),Color(232,13,15,13),0.f);g.FillRectangle(&shade,0.f,0.f,w,h);
 LinearGradientBrush foot(RectF(0,h-270,w,270),Color(0,8,10,8),Color(230,8,10,8),90.f);g.FillRectangle(&foot,0.f,h-270,w,270.f);
 Color white(255,238,238,225),muted(255,172,179,165),quiet(255,112,123,109),accent(255,199,208,183);FontFamily family(L"Segoe UI");
 auto text=[&](REAL x,REAL y,REAL size,Color c,const wchar_t*s,REAL ww=900.f,REAL hh=64.f,StringAlignment align=StringAlignmentNear){Font f(&family,size,FontStyleRegular,UnitPixel);SolidBrush b(c);StringFormat fmt;fmt.SetAlignment(align);g.DrawString(s,-1,&f,RectF(x,y,ww,hh),&fmt,&b);};
 Pen rule(Color(90,189,200,177));g.DrawLine(&rule,56.f,69.f,w-56,69.f);
 text(56,32,16,white,L"inFAMOUS / PC");text(w-380,34,12,muted,L"GRAPHICS INITIALIZATION",324,30,StringAlignmentFar);
 text(56,89,12,quiet,first_light?L"FIRST LIGHT":L"SECOND SON");
 REAL x=w*.51f,y=std::max(164.f,h*.30f),body=w-x-72;
 text(x,y-30,12,accent,pt?L"PREPARAÇÃO DO SISTEMA":L"SYSTEM PREPARATION",body);
 const wchar_t*title=phase==0?(pt?L"Iniciando":L"Starting"):phase==5?(pt?L"Indexando shaders":L"Indexing shaders"):phase==4?(pt?L"Compilando shaders":L"Compiling shaders"):phase==1?(pt?L"Carregando shaders":L"Loading shader cache"):phase==3?(pt?L"Finalizando os gráficos":L"Finishing graphics"):(pt?L"Preparando os gráficos":L"Preparing graphics");
 text(x-2,y,34,white,title,body+4,58);
 text(x,y+69,18,muted,pt?L"A compilação de shaders sempre demora mais na primeira execução após uma atualização...":L"Compiling Shaders always takes the longest on the first run after updating...",body,110);
 text(x,y+180,12,quiet,pt?L"AGUARDE. O JOGO INICIARÁ AUTOMATICAMENTE.":L"PLEASE WAIT. YOUR GAME WILL START AUTOMATICALLY.",body,55);
 bool known=total>0;done=known?std::min(done,total):0;REAL by=h-128,bw=w-112;
 text(56,by-38,12,accent,phase==5?L"SHADER INDEX":phase==4?L"SHADER COMPILATION":phase==1?L"SHADER CACHE":L"GRAPHICS PREPARATION",bw-180,32);
 wchar_t percent[24];if(known)swprintf(percent,24,L"%u%%",unsigned(static_cast<unsigned long long>(done)*100/total));else swprintf(percent,24,L"--%%");
 text(w-260,by-62,42,white,percent,204,60,StringAlignmentFar);
 SolidBrush track(Color(95,104,114,98)),fill(accent);g.FillRectangle(&track,56.f,by,bw,6.f);
 if(known&&done)g.FillRectangle(&fill,56.f,by,bw*REAL(done)/total,6.f);
 else if(!known){REAL t=REAL(ticks%5000)/5000.f,l=std::max(0.f,t*1.2f-.2f),r=std::min(1.f,t*1.2f);if(r>l)g.FillRectangle(&fill,56+bw*l,by,bw*(r-l),6.f);}
 Pen edge(Color(160,210,217,198));g.DrawLine(&edge,56.f,by+8,w-56,by+8);
 wchar_t count[96];if(known)swprintf(count,96,L"%u / %u shaders",done,total);else swprintf(count,96,L"%ls",pt?L"Aguardando o total de shaders":L"Waiting for shader totals");text(56,by+20,14,muted,count,bw*.55f,32);
 const wchar_t*status=pt?L"Preparando a próxima etapa":L"Preparing the next stage";wchar_t estimate[96];
 if((phase==4||phase==5)&&known&&done>eta_from&&done<total&&elapsed>=3&&std::isfinite(elapsed)){unsigned left=unsigned(std::min(86400.,elapsed*double(total-done)/double(done-eta_from)))+1;swprintf(estimate,96,pt?L"Tempo estimado restante  %u:%02u":L"Estimated time remaining  %u:%02u",left/60,left%60);status=estimate;}
 else if(known&&done==total)status=pt?L"Etapa concluída":L"Stage complete";
 text(w*.5f,by+20,14,muted,status,w*.5f-56,32,StringAlignmentFar);
 text(56,h-48,13,quiet,pt?L"Pressione F1 no jogo para abrir o Overlay ou R3+L2/LT no controle":L"Press F1 in-game to get the Overlay to show up or press R3+L2/LT on controller",bw,32);
 g.Restore(saved);
}
}
