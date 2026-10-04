#include "draw.h"
#include "plasma_data.h"
void draw_pixel(UiFramebuffer *f,int x,int y,UiColor c){if(x<0||y<0||x>=f->width||y>=f->height||!c.a)return;unsigned char *d=f->pixels+y*f->stride+x*4;if(c.a==255){d[0]=c.r;d[1]=c.g;d[2]=c.b;d[3]=255;return;}if(d[3]==255){d[0]=(c.r*c.a+d[0]*(255-c.a)+127)/255;d[1]=(c.g*c.a+d[1]*(255-c.a)+127)/255;d[2]=(c.b*c.a+d[2]*(255-c.a)+127)/255;return;}unsigned inv=255-c.a,den=c.a*255+d[3]*inv;if(!den)return;d[0]=(c.r*c.a*255+d[0]*d[3]*inv)/den;d[1]=(c.g*c.a*255+d[1]*d[3]*inv)/den;d[2]=(c.b*c.a*255+d[2]*d[3]*inv)/den;d[3]=(den+127)/255;}
static int round_coverage(int x,int y,int w,int h,int radius){
    if(!radius)return 255;int r=radius*16,px=x*16+8,py=y*16+8;
    int dx=px<r?r-px:px>(w*16-r)?px-(w*16-r):0,dy=py<r?r-py:py>(h*16-r)?py-(h*16-r):0;
    int d=dx*dx+dy*dy;if(d<r*r-r*24)return 255;if(d>r*r+r*24)return 0;
    int count=0;for(int yy=2;yy<16;yy+=4)for(int xx=2;xx<16;xx+=4){px=x*16+xx;py=y*16+yy;dx=px<r?r-px:px>(w*16-r)?px-(w*16-r):0;dy=py<r?r-py:py>(h*16-r)?py-(h*16-r):0;count+=dx*dx+dy*dy<=r*r;}return (count*255+8)/16;
}
void draw_round(UiFramebuffer *f,int x,int y,int w,int h,int radius,UiColor top,UiColor bottom){if(w<=0||h<=0)return;if(radius<0)radius=0;if(radius>w/2)radius=w/2;if(radius>h/2)radius=h/2;for(int yy=0;yy<h;yy++){UiColor c={(top.r*(h-yy)+bottom.r*yy)/h,(top.g*(h-yy)+bottom.g*yy)/h,(top.b*(h-yy)+bottom.b*yy)/h,(top.a*(h-yy)+bottom.a*yy)/h};for(int xx=0;xx<w;xx++){int coverage=round_coverage(xx,yy,w,h,radius);if(coverage){UiColor z=c;z.a=(z.a*coverage+127)/255;draw_pixel(f,x+xx,y+yy,z);}}}}
void draw_circle(UiFramebuffer *f,int cx,int cy,int radius,UiColor c){if(radius<1){draw_pixel(f,cx,cy,c);return;}int r=radius*16;for(int y=-radius;y<=radius;y++)for(int x=-radius;x<=radius;x++){int d=(x*16+8)*(x*16+8)+(y*16+8)*(y*16+8),coverage=0;if(d<r*r-r*24)coverage=255;else if(d<=r*r+r*24){int count=0;for(int yy=2;yy<16;yy+=4)for(int xx=2;xx<16;xx+=4)count+=(x*16+xx)*(x*16+xx)+(y*16+yy)*(y*16+yy)<=r*r;coverage=(count*255+8)/16;}UiColor z=c;z.a=z.a*coverage/255;draw_pixel(f,cx+x,cy+y,z);}}
void draw_line(UiFramebuffer *f,int x,int y,int ex,int ey,int thickness,UiColor c){int dx=ex-x,dy=ey-y;int n=abs(dx)>abs(dy)?abs(dx):abs(dy);if(!n)n=1;for(int i=0;i<=n;i++)draw_circle(f,x+dx*i/n,y+dy*i/n,thickness,c);}
void draw_triangle(UiFramebuffer *f,int ax,int ay,int bx,int by,int cx,int cy,UiColor c){int minx=ax<bx?ax:bx;if(cx<minx)minx=cx;int maxx=ax>bx?ax:bx;if(cx>maxx)maxx=cx;int miny=ay<by?ay:by;if(cy<miny)miny=cy;int maxy=ay>by?ay:by;if(cy>maxy)maxy=cy;for(int y=miny;y<=maxy;y++)for(int x=minx;x<=maxx;x++){int a=(x-ax)*(by-ay)-(y-ay)*(bx-ax),b=(x-bx)*(cy-by)-(y-by)*(cx-bx),d=(x-cx)*(ay-cy)-(y-cy)*(ax-cx);if((a>=0&&b>=0&&d>=0)||(a<=0&&b<=0&&d<=0))draw_pixel(f,x,y,c);}}
void draw_background(UiFramebuffer *f,uint64_t now){
    enum {STEP=UI_BACKGROUND_STEP,GW=UI_WIDTH/STEP+1,GH=UI_HEIGHT/STEP+1};
    int grid[GH][GW];int phase=(int)((now%UI_BACKGROUND_PERIOD)*1024/UI_BACKGROUND_PERIOD);
    for(int y=0;y<GH;y++)for(int x=0;x<GW;x++){
        int px=x*STEP,py=y*STEP;
        /* Keep the field deliberately below the eye's flicker threshold. The
         * coordinate warp creates broad smoke-like folds, instead of a bright
         * plasma wave being clipped to black at the two side vignettes. */
        int wx=px+plasma_sine[(py*2+phase)&1023]/8+plasma_sine[(px+phase/2)&1023]/16;
        int wy=py+plasma_sine[(px*2-phase)&1023]/10+plasma_sine[(py-phase/3)&1023]/20;
        int smoke=plasma_sine[(wx*2+phase)&1023]/4+
                  plasma_sine[(wy*3-phase/2)&1023]/6+
                  plasma_sine[(wx+wy+phase)&1023]/10;
        int dx=px-UI_WIDTH/2,dy=py-UI_HEIGHT/2;
        int vignette=(dx*dx/90000+dy*dy/65000)*256;
        int value=7*256+smoke-vignette;
        if(value<2*256)value=2*256;if(value>15*256)value=15*256;grid[y][x]=value;
    }
    for(int y=0;y<UI_HEIGHT;y++)for(int x=0;x<UI_WIDTH;x++){
        int gy=y/STEP,gx=x/STEP,fy=y%STEP,fx=x%STEP;
        int top=grid[gy][gx]*(STEP-fx)+grid[gy][gx+1]*fx,bottom=grid[gy+1][gx]*(STEP-fx)+grid[gy+1][gx+1]*fx;
        int value=(top*(STEP-fy)+bottom*fy)/(STEP*STEP);int v=(value+128)/256;
        unsigned char *p=f->pixels+y*f->stride+x*4;p[0]=p[1]=p[2]=(unsigned char)v;p[3]=255;
    }
}
