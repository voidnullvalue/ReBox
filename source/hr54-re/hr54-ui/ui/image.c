#include "image.h"
void image_scale(const unsigned char *src,int sw,int sh,unsigned char *dst,int dw,int dh){
    if(!src||!dst||sw<1||sh<1||dw<1||dh<1)return;
    for(int y=0;y<dh;y++)for(int x=0;x<dw;x++){
        uint64_t rgba[4]={0},weight=0;
        if(dw<sw||dh<sh){
            int x0=x*sw*256/dw,x1=(x+1)*sw*256/dw,y0=y*sh*256/dh,y1=(y+1)*sh*256/dh;
            for(int sy=y0/256;sy<(y1+255)/256;sy++)for(int sx=x0/256;sx<(x1+255)/256;sx++){
                int left=x0>sx*256?x0:sx*256,right=x1<(sx+1)*256?x1:(sx+1)*256;
                int top=y0>sy*256?y0:sy*256,bottom=y1<(sy+1)*256?y1:(sy+1)*256;
                unsigned w=(right-left)*(bottom-top);const unsigned char *p=src+(sy*sw+sx)*4;
                weight+=w;rgba[3]+=(uint64_t)p[3]*w;for(int c=0;c<3;c++)rgba[c]+=(uint64_t)p[c]*p[3]*w;
            }
        }else{
            int fx=((2*x+1)*sw*128/dw)-128,fy=((2*y+1)*sh*128/dh)-128;if(fx<0)fx=0;if(fy<0)fy=0;
            int sx=fx/256,sy=fy/256,nx=sx+1<sw?sx+1:sx,ny=sy+1<sh?sy+1:sy;fx%=256;fy%=256;
            int xx[]={sx,nx,sx,nx},yy[]={sy,sy,ny,ny},ww[]={(256-fx)*(256-fy),fx*(256-fy),(256-fx)*fy,fx*fy};
            for(int i=0;i<4;i++){const unsigned char *p=src+(yy[i]*sw+xx[i])*4;weight+=ww[i];rgba[3]+=(uint64_t)p[3]*ww[i];for(int c=0;c<3;c++)rgba[c]+=(uint64_t)p[c]*p[3]*ww[i];}
        }
        unsigned char *d=dst+(y*dw+x)*4;d[3]=(rgba[3]+weight/2)/weight;
        for(int c=0;c<3;c++)d[c]=rgba[3]?(rgba[c]+rgba[3]/2)/rgba[3]:0;
    }
}
void image_draw(UiFramebuffer *f,const unsigned char *src,int w,int h,int x,int y,int opacity){
    if(!src)return;if(opacity<0)opacity=0;if(opacity>255)opacity=255;
    for(int yy=0;yy<h;yy++)for(int xx=0;xx<w;xx++){const unsigned char *p=src+(yy*w+xx)*4;draw_pixel(f,x+xx,y+yy,COLOR(p[0],p[1],p[2],(p[3]*opacity+127)/255));}
}
