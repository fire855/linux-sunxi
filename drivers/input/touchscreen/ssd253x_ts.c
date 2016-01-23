#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/timer.h>
#include <linux/delay.h>
#include <linux/hrtimer.h>
#include <linux/i2c.h>
#include <asm/io.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/ioport.h>

#include <mach/irqs.h>

#include <mach/system.h>
#include <mach/hardware.h>
#include <plat/sys_config.h>
#include "ctp_platform_ops.h"

#include "ssd253x_ts.h"

#ifdef CONFIG_HAS_EARLYSUSPEND
#include <linux/earlysuspend.h>
#endif /* CONFIG_HAS_EARLYSUSPEND */

//#define CONFIG_TOUCHSCREEN_SSL_DEBUG	
#undef  CONFIG_TOUCHSCREEN_SSL_DEBUG

#define DEVICE_ID_REG                    2
#define VERSION_ID_REG                 3
#define AUTO_INIT_RST_REG          68
#define EVENT_STATUS                   121
#define EVENT_MSK_REG                 122
#define IRQ_MSK_REG                     123
#define FINGER01_REG                    124
#define EVENT_STACK                   	 128
#define EVENT_FIFO_SCLR               135
#define TIMESTAMP_REG                 136
#define SELFCAP_STATUS_REG         185		

#define VERSION 	"ssd253x_20121016_17:00 SSD2533.Charles@Raysens"

#define	TOUCH_INT_NO	SW_INT_IRQNO_PIO 
#define SSD253X_I2C_NAME	"ssd253x_ts"
#define CTP_NAME			SSD253X_I2C_NAME
#define CTP_IIC_ADDR        0x48
static __u32 twi_addr = 0;
static __u32 twi_id = 0; //by jicky
/*
struct ChipSetting {
	char No;
	char Reg;
	char Data1;
	char Data2;
};*/

/* Addresses to scan by jicky*/
union{
	unsigned short dirty_addr_buf[2];
	const unsigned short normal_i2c[2];
}u_i2c_addr = {{0x00},};


static void* __iomem gpio_addr = NULL;
static int gpio_int_hdle = 0;
static int gpio_wakeup_hdle = 0;
static int gpio_en_hdle = 0;
static int screen_max_x;
static int screen_max_y;
static int revert_x_flag = 0;
static int revert_y_flag = 0;
static int exchange_x_y_flag = 0;
static int ssd253x_version;

static int  ctp_type=0;

void deviceReset(struct i2c_client *client);
void deviceResume(struct i2c_client *client);
void deviceSuspend(struct i2c_client *client);
void SSD253xdeviceInit1(struct i2c_client *client);
void SSD253xdeviceInit(struct i2c_client *client); 

static int ssd253x_ts_open(struct input_dev *dev);
static void ssd253x_ts_close(struct input_dev *dev);
static int ssd253x_ts_suspend(struct i2c_client *client, pm_message_t mesg);
static int ssd253x_ts_resume(struct i2c_client *client);
#ifdef CONFIG_HAS_EARLYSUSPEND
static void ssd253x_ts_early_suspend(struct early_suspend *h);
static void ssd253x_ts_late_resume(struct early_suspend *h);
#endif /* CONFIG_HAS_EARLYSUSPEND */

static enum hrtimer_restart ssd253x_ts_timer(struct hrtimer *timer);
static irqreturn_t ssd253x_ts_isr(int irq, void *dev_id);
static struct workqueue_struct *ssd253x_wq;

struct ssl_ts_priv {
	struct i2c_client *client;
	struct input_dev *input;
	struct hrtimer timer;
	struct work_struct  ssl_work;
#ifdef	CONFIG_HAS_EARLYSUSPEND
	struct early_suspend early_suspend;
#endif 

	int irq;
	int use_irq;
	int FingerNo;
	int FingerX[FINGERNO];
	int FingerY[FINGERNO];
	int FingerP[FINGERNO];

	int Resolution;
	int EventStatus;
	int FingerDetect;

	int sFingerX[FINGERNO];
	int sFingerY[FINGERNO];
	int pFingerX[FINGERNO];
	int pFingerY[FINGERNO];
};

int ssd253x_record,ssd253x_current,ssd253x_record_times=0,ssd253x_timer_flag=0; //add by hjc

int ReadRegister(struct i2c_client *client,uint8_t reg,int ByteNo)
{
	unsigned char buf[4];
	struct i2c_msg msg[2];
	int ret;

	memset(buf, 0xFF, sizeof(buf));
	msg[0].addr = client->addr;
	msg[0].flags = 0;
	msg[0].len = 1;
	msg[0].buf = &reg;

	msg[1].addr = client->addr;
	msg[1].flags = I2C_M_RD;
	msg[1].len = ByteNo;
	msg[1].buf = buf;

	if(i2c_transfer(client->adapter, msg, 2)<0)
    	{
	    printk("		ReadRegister: i2c_transfer Error !\n");
	    return -1;
	}
	    
	if(ByteNo==1) return (int)((unsigned int)buf[0]<<0);
	if(ByteNo==2) return (int)((unsigned int)buf[1]<<0)|((unsigned int)buf[0]<<8);
	if(ByteNo==3) return (int)((unsigned int)buf[2]<<0)|((unsigned int)buf[1]<<8)|((unsigned int)buf[0]<<16);
	if(ByteNo==4) return (int)((unsigned int)buf[3]<<0)|((unsigned int)buf[2]<<8)|((unsigned int)buf[1]<<16)|(buf[0]<<24);
	
	return 0;
}

void WriteRegister(struct i2c_client *client,uint8_t Reg,unsigned char Data1,unsigned char Data2,int ByteNo)
{	
	struct i2c_msg msg;
	unsigned char buf[4];
	
	buf[0]=Reg;
	buf[1]=Data1;
	buf[2]=Data2;
	buf[3]=0;

	msg.addr = client->addr;
	msg.flags = 0;
	msg.len = ByteNo+1;
	msg.buf = (char *)buf;
	
	if(i2c_transfer(client->adapter, &msg, 1)<0)
		printk("WriteRegister: i2c_master_send Error !\n");

}

void SSD253xdeviceInit(struct i2c_client *client)
{	
	int i;
	
	if ( (1024 == screen_max_x) && (768 == screen_max_y))			//9.7 inch SSD2533QN10 TP
	{	
		for(i=0;i<sizeof(ssd253xcfgTable9_7)/sizeof(ssd253xcfgTable9_7[0]);i++)
		{
			WriteRegister(	client,ssd253xcfgTable9_7[i].Reg,
					ssd253xcfgTable9_7[i].Data1,ssd253xcfgTable9_7[i].Data2,
					ssd253xcfgTable9_7[i].No);
		}
	}
	else if ( (1024 == screen_max_x) && (600 == screen_max_y))//10.1 inch, Raysens RS10MD258 SSD2533QN10 TP
	{
			
		struct ChipSetting *pSsd2533_cfg=NULL;
		int reg_num=0;
		
		if(ctp_type==0)
		{	
			pSsd2533_cfg=(ssd253x_version==0x0103)?ssd2533QN10_RS258_YF_cfg:ssd2533QN10_RS258_YF_ES5_cfg;
			reg_num=(ssd253x_version==0x0103)?(sizeof(ssd2533QN10_RS258_YF_cfg)/sizeof(ssd2533QN10_RS258_YF_cfg[0]))
							 :(sizeof(ssd2533QN10_RS258_YF_ES5_cfg)/sizeof(ssd2533QN10_RS258_YF_ES5_cfg[0]));
		}else if(ctp_type==1)
		{	//��ƽ
			pSsd2533_cfg=(ssd253x_version==0x0103)?ssd2533QN10_RS258_YF_cfg_flat:ssd2533QN10_RS258_YF_ES5_cfg_flat;
			reg_num=(ssd253x_version==0x0103)?(sizeof(ssd2533QN10_RS258_YF_cfg_flat)/sizeof(ssd2533QN10_RS258_YF_cfg_flat[0]))
							 :(sizeof(ssd2533QN10_RS258_YF_ES5_cfg_flat)/sizeof(ssd2533QN10_RS258_YF_ES5_cfg_flat[0]));
		}

		for(i=0;i<reg_num;i++)
		{
			WriteRegister(	client,
					pSsd2533_cfg[i].Reg,
					pSsd2533_cfg[i].Data1,
					pSsd2533_cfg[i].Data2,
					pSsd2533_cfg[i].No);
		}
			
	}
	
}

void deviceReset(struct i2c_client *client)
{	
	int i;
	for(i=0;i<sizeof(Reset)/sizeof(Reset[0]);i++)
	{
		WriteRegister(	client,Reset[i].Reg,
				Reset[i].Data1,Reset[i].Data2,
				Reset[i].No);
	}
	mdelay(10);
}

void deviceResume(struct i2c_client *client)    
{	
	#if 1
	//RST pin pull down
	gpio_write_one_pin_value(gpio_wakeup_hdle, 0, "ctp_wakeup");
	mdelay(5);
	gpio_write_one_pin_value(gpio_wakeup_hdle, 1, "ctp_wakeup");
	mdelay(2);

	deviceReset(client);
	SSD253xdeviceInit(client);
	#else
	int i;
	for(i=0;i<sizeof(Resume)/sizeof(Resume[0]);i++)
	{
		WriteRegister(	client,Resume[i].Reg,
				Resume[i].Data1,Resume[i].Data2,
				Resume[i].No);
		mdelay(150);
	}
	#endif
	
}

void deviceSuspend(struct i2c_client *client)
{	
	#if 1
	//RST pin pull down
	gpio_write_one_pin_value(gpio_wakeup_hdle, 1, "ctp_wakeup");
	mdelay(5);
	gpio_write_one_pin_value(gpio_wakeup_hdle, 0, "ctp_wakeup");
	mdelay(2);
	#else
	//int i;
	//int timeout=10;
	//int status;
	
	/*
	WriteRegister(	client,Suspend[0].Reg,
			Suspend[0].Data1,Suspend[0].Data2,
			Suspend[0].No);
	do {
		status=ReadRegister(client,0x26,1);
		if(status==Suspend[0].Data1) break;
		mdelay(1);				
	}while(timeout--);
	*/
	
	for(i=0;i<sizeof(Suspend)/sizeof(Suspend[0]);i++)
	{
		WriteRegister(	client,Suspend[i].Reg,
				Suspend[i].Data1,Suspend[i].Data2,
				Suspend[i].No);
		mdelay(200);
	}
	#endif
}

#define Mode RunningAverageMode
#define Dist RunningAverageDist
void RunningAverage(unsigned short *xpos,unsigned short *ypos,int No,struct ssl_ts_priv *ssl_priv)
{	
	int FilterMode[4][2]={{0,8},{5,3},{6,2},{7,1}};
	int dx,dy;
	int X,Y;

	X=*xpos;
	Y=*ypos;
	if((ssl_priv->pFingerX[No]!=0x0FFF)&&(X!=0x0FFF))
	{
		dx=abs(ssl_priv->pFingerX[No]-X);
		dy=abs(ssl_priv->pFingerY[No]-Y);
		if(dx+dy<Dist*64)
		{
			ssl_priv->pFingerX[No]=(FilterMode[Mode][0]*ssl_priv->pFingerX[No]+FilterMode[Mode][1]*X)/8;
			ssl_priv->pFingerY[No]=(FilterMode[Mode][0]*ssl_priv->pFingerY[No]+FilterMode[Mode][1]*Y)/8;
		}
		else
		{
			ssl_priv->pFingerX[No]=X;
			ssl_priv->pFingerY[No]=Y;
		}
	}
	else
	{
		ssl_priv->pFingerX[No]=X;
		ssl_priv->pFingerY[No]=Y;
	}
	*xpos=ssl_priv->pFingerX[No];
	*ypos=ssl_priv->pFingerY[No];
}

void FingerCheckSwap(int *FingerX,int *FingerY,int *FingerP,int FingerNo,int *sFingerX,int *sFingerY)
{
  	int i,j;
  	int index1,index2;
  	int Vx,Vy;
  	int Ux,Uy;
  	int R1x,R1y;
  	int R2x,R2y;
	for(i=0;i<FingerNo;i++)
  	{
 		index1=i;
	    	if( FingerX[index1]!=0xFFF)
		if(sFingerX[index1]!=0xFFF) 
		{
			for(j=i+1;j<FingerNo+3;j++)
			{
				index2=j%FingerNo;
	    			if( FingerX[index2]!=0xFFF)
				if(sFingerX[index2]!=0xFFF) 
		    		{
					Ux=sFingerX[index1]-sFingerX[index2];
					Uy=sFingerY[index1]-sFingerY[index2];      
					Vx= FingerX[index1]- FingerX[index2];
					Vy= FingerY[index1]- FingerY[index2];					

					R1x=Ux-Vx;
					R1y=Uy-Vy;
					R2x=Ux+Vx;
					R2y=Uy+Vy;
							
					R1x=R1x*R1x;
					R1y=R1y*R1y; 
					R2x=R2x*R2x;
					R2y=R2y*R2y;

					if(R1x+R1y>R2x+R2y)
				    	{
				    		Ux=FingerX[index1];
						Uy=FingerY[index1];
						Vx=FingerP[index1];
							          
						FingerX[index1]=FingerX[index2];
						FingerY[index1]=FingerY[index2];
						FingerP[index1]=FingerP[index2];
							
						FingerX[index2]=Ux;
						FingerY[index2]=Uy;
						FingerP[index2]=Vx;
					}
					break;
			    	}
			}
		}
  	}        
  	for(i=0;i<FingerNo;i++)
  	{
    		sFingerX[i]=FingerX[i];
    		sFingerY[i]=FingerY[i];
  	}
}


#ifdef SSD253x_CUT_EDGE
static int ssd253x_ts_cut_edge(unsigned short pos,unsigned short x_y)
{
	u8 cut_value = 10; //cut_value < 32
	if(pos == 0xfff)
	{
		return pos;
	}
	if(x_y) //xpos
	{
		if(pos < 16)
			pos = cut_value + pos*(48 - cut_value) / 16;
		else if(pos > (XPOS_MAX - 16) )
			pos = XPOS_MAX + 16 + (pos - (XPOS_MAX -16))*(48 - cut_value) / 16;
		else
			pos = pos + 32;

		pos = SSDS53X_SCREEN_MAX_X * pos / (SENSENO * 64);
		return pos;
	}
	else    //ypos
	{
		if(pos < 16)
			pos = cut_value + pos*(48 - cut_value) / 16;
		else if(pos > (YPOS_MAX - 16) )
			pos = YPOS_MAX + 16 + (pos - (YPOS_MAX -16))*(48 - cut_value) / 16;
		else
			pos = pos + 32;
		
		pos = SSDS53X_SCREEN_MAX_Y* pos / (DRIVENO * 64);
		return pos;		
	}
	
	
}
#endif

static void ssd253x_ts_work(struct work_struct *work)
{
	int i;
	unsigned short xpos=0, ypos=0, width=0;
	int FingerInfo;
	int EventStatus;
	int FingerX[FINGERNO];
	int FingerY[FINGERNO];
	int FingerP[FINGERNO];
	int clrFlag=0;
	int timer_status;
	#ifdef SSD253x_TOUCH_KEY
	u8 btn_status;
	static bool key[4] = { 0, 0, 0, 0 }; 
	#endif
	

	struct ssl_ts_priv *ssl_priv = container_of(work,struct ssl_ts_priv,ssl_work);
	
	#ifdef CONFIG_TOUCHSCREEN_SSL_DEBUG
	printk("|	ssd253x_ts_work!                  |\n");
	#endif

		timer_status = ReadRegister(ssl_priv->client,TIMESTAMP_REG,2);
		if(!ssd253x_record)                                      
		{
				ssd253x_record = timer_status/1000;   			
		}
		
		ssd253x_current = timer_status/1000; 
		
		if(ssd253x_current < ssd253x_record)
       		{
       			ssd253x_current += 0xffff/1000;
       		}       
		
		if((ssd253x_current - ssd253x_record) > 10)		//charles change, 120827
		{
		ssd253x_record = 0;
		ssd253x_current = 0;
		ssd253x_record_times++;
		}
		
		if((ssd253x_record_times == 1) && (ssd253x_timer_flag == 0))
		{
			printk("******** (ssd253x_record_times == 1) && (ssd253x_timer_flag == 0) *******\n ");	
			ssd253x_timer_flag = 1;
			WriteRegister(ssl_priv->client,AUTO_INIT_RST_REG,0x00,0x00,1);
	 	}

	#ifdef SSD253x_TOUCH_KEY
		KeyInfo = ReadRegister(ssl_priv->client,0xB9,1);
		#ifdef CONFIG_TOUCHSCREEN_SSL_DEBUG
		if(KeyInfo<0)	printk("		ssd253x_ts_work: i2c_transfer Error !\n");
		#endif
		#ifdef CONFIG_TOUCHSCREEN_SSL_DEBUG
		printk("ssd253x_ts_work read 0xB9,KeyInfo is %x\n",KeyInfo);
		#endif
		if(KeyInfo & 0x0f){
			switch(KeyInfo & 0x0f){
			case 1:
			key[0] = 1;
			input_event(ssl_priv->input,EV_KEY, key_code[0], 1);
			break;
			case 2:
			key[1] = 1;
			input_event(ssl_priv->input,EV_KEY, key_code[1], 1);
			break;
			case 4:
			key[2] = 1;
			input_event(ssl_priv->input,EV_KEY, key_code[2], 1);
			break;
			case 8:
			key[3] = 1;
			input_event(ssl_priv->input,EV_KEY, key_code[3], 1);
			break;
			default:
			break;
			}
			hrtimer_start(&ssl_priv->timer, ktime_set(0, MicroTimeTInterupt), HRTIMER_MODE_REL);
		goto work_touch;
		}
		for(i = 0; i < 4; i++)
		{
			if(key[i])
			{
				key[i] = 0;
				input_event(ssl_priv->input, EV_KEY, key_code[i], 0);
			}
		}
	work_touch:
	#endif

	//EventStatus = ReadRegister(ssl_priv->client,EVENT_STATUS,2)>>4;

	EventStatus = ReadRegister(ssl_priv->client,EVENT_STATUS,2);
	//printk("		ssd253x_ts_work: EventStatus = 0x%x \n",EventStatus);
	//Info =   EventStatus;  
	if(EventStatus == 0)
	{
		ssd253x_record_times = 0;		
	}  

	//if(ssd253x_record_times > 6)
	{
		if((((EventStatus >> 14) & 0x3) || ((EventStatus >> 3) & 0x1)) && ssd253x_record_times > 6 )
		{
		  	printk("******** ssd253x_ts_work: Calibration!!!! *******\n ");	
			mdelay(200);	
			WriteRegister(ssl_priv->client,0xa2,0x01,0x00,1);
			ssd253x_record_times = 0;
			mdelay(200);
			EventStatus = 0;
		
		}
	}
	
	EventStatus = EventStatus >> 4; 

	ssl_priv->FingerDetect=0;
	for(i=0;i<ssl_priv->FingerNo;i++)
	{
		if((EventStatus>>i)&0x1)
		{ 
			FingerInfo=ReadRegister(ssl_priv->client,FINGER01_REG+i,4); 
			xpos = ((FingerInfo>>4)&0xF00)|((FingerInfo>>24)&0xFF);
			ypos = ((FingerInfo>>0)&0xF00)|((FingerInfo>>16)&0xFF);
			width= ((FingerInfo>>4)&0x00F);	
			//printk("before		ssd253x_ts_work: width = %d \n",width);//by jicky
     			//printk("before		ssd253x_ts_work: X = 0x%d , Y = 0x%d\n",xpos,ypos);//by jicky
			if(xpos!=0xFFF)
			{
				ssl_priv->FingerDetect++;

				#ifdef SSD253x_CUT_EDGE
				xpos = ssd253x_ts_cut_edge(xpos, 1);
				ypos = ssd253x_ts_cut_edge(ypos, 0);
				#endif
			}
			else 
			{
				// This part is to avoid asyn problem when the finger leaves
				//printk("		ssd253x_ts_work: Correct %x\n",EventStatus);
				EventStatus=EventStatus&~(1<<i);
				clrFlag=1;
			}
		}
		else
		{
			xpos=ypos=0xFFF;
			width=0;
			clrFlag=1;
		}
		FingerX[i]=xpos;
		FingerY[i]=ypos;
		FingerP[i]=width;
	}
	if(ssl_priv->use_irq==1) ;//enable_irq(ssl_priv->irq);
	if(ssl_priv->use_irq==2)
	{
		if(ssl_priv->FingerDetect==0) ;//enable_irq(ssl_priv->irq);
		else hrtimer_start(&ssl_priv->timer, ktime_set(0, MicroTimeTInterupt), HRTIMER_MODE_REL);
	}
	if(clrFlag) WriteRegister(ssl_priv->client,EVENT_FIFO_SCLR,0x01,0x00,1);

	if(ssl_priv->input->id.product==0x2533)
	if(ssl_priv->input->id.version==0x0101) 
		FingerCheckSwap(FingerX,FingerY,FingerP,ssl_priv->FingerNo,ssl_priv->sFingerX,ssl_priv->sFingerY);

	for(i=0;i<ssl_priv->FingerNo;i++)
	{
		xpos=FingerX[i];
		ypos=FingerY[i];
	width=FingerP[i];
		if(ssl_priv->input->id.product==0x2533)
		{
			if(ssl_priv->input->id.version==0x0101) RunningAverage(&xpos,&ypos,i,ssl_priv);
			if(ssl_priv->input->id.version==0x0102) RunningAverage(&xpos,&ypos,i,ssl_priv);
		}

		if(xpos!=0xFFF)
		{
		//	 printk("report		ssd253x_ts_work: X = 0x%d , Y = 0x%d\n",xpos,ypos);//by jicky
		  //input_report_key(ssl_priv->input, BTN_TOUCH,  1);
			input_report_abs(ssl_priv->input, ABS_MT_TRACKING_ID, i);  
			input_report_abs(ssl_priv->input, ABS_MT_TOUCH_MAJOR, 3);
			input_report_abs(ssl_priv->input, ABS_MT_POSITION_X, xpos);
			input_report_abs(ssl_priv->input, ABS_MT_POSITION_Y, ypos);
			//input_report_abs(ssl_priv->input, ABS_MT_WIDTH_MAJOR, width);//width);
			input_mt_sync(ssl_priv->input);

			//#ifdef CONFIG_TOUCHSCREEN_SSL_DEBUG
			#if 0 //jicky
			if(i==0) printk("		ssd253x_ts_work: X = 0x%d , Y = 0x%d, W = 0x%d\n",xpos,ypos,width);
			#endif
		}
		else if(ssl_priv->FingerX[i]!=0xFFF)
		{
			//input_report_key(ssl_priv->input, BTN_TOUCH,  0);
			input_report_abs(ssl_priv->input, ABS_MT_TRACKING_ID, i);
			//input_report_abs(ssl_priv->input, ABS_MT_POSITION_X, ssl_priv->FingerX[i]);
			//input_report_abs(ssl_priv->input, ABS_MT_POSITION_Y, ssl_priv->FingerY[i]);
			input_report_abs(ssl_priv->input, ABS_MT_TOUCH_MAJOR, 0);
			input_mt_sync(ssl_priv->input);
			#ifdef CONFIG_TOUCHSCREEN_SSL_DEBUG
			if(i==0) printk("	release	ssd253x_ts_work: X = 0x%x , Y = 0x%x, W = 0x%x\n",xpos,ypos,width);
			#endif
		}
		ssl_priv->FingerX[i]=FingerX[i];
		ssl_priv->FingerY[i]=FingerY[i];
		ssl_priv->FingerP[i]=width;
	}		
	ssl_priv->EventStatus=EventStatus;	
	input_sync(ssl_priv->input);
}

static int ssd253x_ts_probe(struct i2c_client *client,const struct i2c_device_id *idp)
{
	struct ssl_ts_priv *ssl_priv;
	struct input_dev *ssl_input;
	int error;
	int i,count=5;
    	int reg_val, err;

	printk("ssd253x_ts_probe\n");
	
	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -ENODEV;
		
    	gpio_wakeup_hdle = gpio_request_ex("ctp_para", "ctp_wakeup");
	if(!gpio_wakeup_hdle) {
		pr_warning("touch panel tp_wakeup request gpio fail!\n");
		goto err_wakeup_request;
	}	
	
	gpio_write_one_pin_value(gpio_wakeup_hdle, 1, "ctp_wakeup");
	mdelay(10);
	gpio_write_one_pin_value(gpio_wakeup_hdle, 0, "ctp_wakeup");	
    	mdelay(10);	
	gpio_write_one_pin_value(gpio_wakeup_hdle, 1, "ctp_wakeup");	
    	mdelay(100);	
   
   	deviceReset(client);
    	while(count--)
    	{
       		mdelay(10);
        	if(ReadRegister(client, DEVICE_ID_REG,2)>=0)
           		 break;
        	if(count<=1)
            		return -ENODEV;
    	}
	ssd253x_wq = create_singlethread_workqueue("ssd253x_wq");
	if (!ssd253x_wq)
	{
		printk("ssd253x_ts_init: create_singlethread_workqueue Error!\n");
		return -ENOMEM;
	}
	
	ssl_priv = kzalloc(sizeof(*ssl_priv), GFP_KERNEL);
	if (!ssl_priv)
	{
		error=-ENODEV;
		goto	err0;
	}
		
	gpio_addr = ioremap(PIO_BASE_ADDRESS, PIO_RANGE_SIZE);
	if(!gpio_addr) {
	    err = -EIO;
	    goto err_ioremap_failed;	
	}
		
#ifdef AW_GPIO_INT_API_ENABLE

#else
        //Config IRQ_EINT21 Negative Edge Interrupt
        printk("goodix_gt818 config IRQ ENT21\n");
        reg_val = readl(gpio_addr + PIO_INT_CFG2_OFFSET);
        reg_val &=(~(7<<20));
        reg_val |=(1<<20);  //by jicky
        writel(reg_val,gpio_addr + PIO_INT_CFG2_OFFSET);
        
        //Enable IRQ_EINT21 of PIO Interrupt
        reg_val = readl(gpio_addr + PIO_INT_CTRL_OFFSET);
        reg_val |=(1<<IRQ_EINT21);
        writel(reg_val,gpio_addr + PIO_INT_CTRL_OFFSET);
#endif	
	
	//config gpio:
	gpio_int_hdle = gpio_request_ex("ctp_para", "ctp_int_port");
	if(!gpio_int_hdle) {
		pr_warning("touch panel IRQ_EINT21_para request gpio fail!\n");
	    goto err_int_request;
	}
	
	dev_set_drvdata(&client->dev, ssl_priv);
	
	ssl_input = input_allocate_device();
	if (!ssl_input)
	{
		printk("ssd253x_ts_probe: input_allocate_device Error\n");
		error=-ENODEV;
		goto	err1;
	}
	

	ssl_input->evbit[0] = BIT_MASK(EV_KEY) | BIT_MASK(EV_ABS) | BIT_MASK(EV_SYN) ;
	ssl_input->keybit[BIT_WORD(BTN_TOUCH)] = BIT_MASK(BTN_TOUCH) | BIT_MASK(BTN_2);
	ssl_input->name = client->name;
	ssl_input->id.bustype = BUS_I2C;
	ssl_input->id.vendor  = 0x2878; // Modify for Vendor ID
	ssl_input->dev.parent = &client->dev;
	//ssl_input->open = ssd253x_ts_open;
	//ssl_input->close = ssd253x_ts_close;

	input_set_drvdata(ssl_input, ssl_priv);
	ssl_priv->client = client;
	ssl_priv->input = ssl_input;
	ssl_priv->use_irq = ENABLE_INT;
	ssl_priv->irq = TOUCH_INT_NO;
	ssl_priv->FingerNo=FINGERNO;
	ssl_priv->Resolution=64;

	for(i=0;i<ssl_priv->FingerNo;i++)
	{
		//ssl_priv->FingerP[i]=0;
		// For Finger Check Swap
		ssl_priv->sFingerX[i]=0xFFF;
		ssl_priv->sFingerY[i]=0xFFF;

		// For Adaptive Running Average
		ssl_priv->pFingerX[i]=0xFFF;
		ssl_priv->pFingerY[i]=0xFFF;
	}

	
	printk("SSL Touchscreen I2C Address: 0x%02X\n",client->addr);
	ssl_input->id.product = ReadRegister(client, DEVICE_ID_REG,2);
	ssl_input->id.version = ReadRegister(client,VERSION_ID_REG,2);
	ssl_input->id.product = ReadRegister(client, DEVICE_ID_REG,2);
	ssl_input->id.version = ReadRegister(client,VERSION_ID_REG,2);
	printk("SSL Touchscreen Device ID  : 0x%04X..\n",ssl_input->id.product);
	printk("SSL Touchscreen Version ID : 0x%04X..\n",ssl_input->id.version);
	ssd253x_version =  ssl_input->id.version ;
	SSD253xdeviceInit(client);
	WriteRegister(client,EVENT_FIFO_SCLR,0x01,0x00,1); // clear Event FiFo
	#ifdef CONFIG_TOUCHSCREEN_SSL_DEBUG
	printk("ssd253X_ts_probe: %04XdeviceInit OK!\n",ssl_input->id.product);
	#endif

	if(ssl_priv->input->id.product==0x2531)		
		ssl_priv->Resolution=32;
	else if(ssl_priv->input->id.product==0x2533)	
		ssl_priv->Resolution=64;
	else
	{
	    printk("ssd253x_ts_probe: ssl_input->id.product Error\n");
		error=-ENODEV;
		goto	err1;
	}

	input_set_abs_params(ssl_input, ABS_MT_TRACKING_ID, 0,10, 0, 0);
	input_set_abs_params(ssl_input, ABS_MT_TOUCH_MAJOR, 0, 15, 0, 0);
	//input_set_abs_params(ssl_input, ABS_MT_WIDTH_MAJOR, 0, 8, 0, 0);
//	input_set_abs_params(ssl_input, ABS_MT_POSITION_X,  0,SSDS53X_SCREEN_MAX_X + 1, 0, 0);
//	input_set_abs_params(ssl_input, ABS_MT_POSITION_Y,  0,SSDS53X_SCREEN_MAX_Y + 1, 0, 0);
		input_set_abs_params(ssl_input, ABS_MT_POSITION_X,  0,screen_max_x + 1, 0, 0);
		input_set_abs_params(ssl_input, ABS_MT_POSITION_Y,  0,screen_max_y + 1, 0, 0);


	#ifdef SSD253x_TOUCH_KEY
	set_bit(KEY_MENU, ssl_input->keybit);
	set_bit(KEY_HOME, ssl_input->keybit);
	set_bit(KEY_BACK, ssl_input->keybit);
	set_bit(KEY_SEARCH, ssl_input->keybit);
	#endif

	INIT_WORK(&ssl_priv->ssl_work, ssd253x_ts_work);
	error = input_register_device(ssl_input);
	if(error)
	{
	    printk("ssd253x_ts_probe: input_register_device input Error!\n");
		error=-ENODEV;
		goto	err1;
	}
	

	if((ssl_priv->use_irq==1)||(ssl_priv->use_irq==2))
	{
		// Options for different interrupt system 
//		error = request_irq(ssl_priv->irq, ssd253x_ts_isr, IRQF_DISABLED|IRQF_TRIGGER_FALLING, client->name,ssl_priv);
//		error = request_irq(ssl_priv->irq, ssd253x_ts_isr, IRQF_TRIGGER_FALLING, client->name,ssl_priv);
		error = request_irq(ssl_priv->irq, ssd253x_ts_isr, IRQF_TRIGGER_FALLING | IRQF_SHARED, client->name,ssl_priv);
		if(error)
		{
			printk("		ssd253x_ts_probe: request_irq Error!\n");
			error=-ENODEV;
			goto err2;
		}
		//enable_irq(ssl_priv->irq);
	}

	if((ssl_priv->use_irq==0)||(ssl_priv->use_irq==2))
	{
		hrtimer_init(&ssl_priv->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
		ssl_priv->timer.function = ssd253x_ts_timer;
	}
	
#ifdef	CONFIG_HAS_EARLYSUSPEND
	ssl_priv->early_suspend.suspend = ssd253x_ts_early_suspend;
	ssl_priv->early_suspend.resume  = ssd253x_ts_late_resume;
	//ssl_priv->early_suspend.level   = EARLY_SUSPEND_LEVEL_BLANK_SCREEN+1;
	ssl_priv->early_suspend.level   =  EARLY_SUSPEND_LEVEL_DISABLE_FB + 1;		//Charles chg 120827
	register_early_suspend(&ssl_priv->early_suspend);
#endif 
    
	return 0;

err2:	input_unregister_device(ssl_input);
err1:	input_free_device(ssl_input);
err_wakeup_request:
err_int_request:	
err_ioremap_failed:
    if(gpio_addr){
        iounmap(gpio_addr);
    }	
	kfree(ssl_priv);
err0:	dev_set_drvdata(&client->dev, NULL);
	return error;
}

static int ssd253x_ts_open(struct input_dev *dev)
{
	struct ssl_ts_priv *ssl_priv = input_get_drvdata(dev);
	#ifdef CONFIG_TOUCHSCREEN_SSL_DEBUG
	printk("|	ssd253x_ts_open!                  |\n");
	#endif	
	deviceResume(ssl_priv->client);
	if(ssl_priv->use_irq) enable_irq(ssl_priv->irq);
	else hrtimer_start(&ssl_priv->timer, ktime_set(1, 0), HRTIMER_MODE_REL);
	return 0;
}

static void ssd253x_ts_close(struct input_dev *dev)
{
	struct ssl_ts_priv *ssl_priv = input_get_drvdata(dev);
	#ifdef CONFIG_TOUCHSCREEN_SSL_DEBUG
	printk("|	ssd253x_ts_close!                 |\n");
	#endif
	deviceSuspend(ssl_priv->client);
	if((ssl_priv->use_irq==0)||(ssl_priv->use_irq==2)) hrtimer_cancel(&ssl_priv->timer);
	if((ssl_priv->use_irq==1)||(ssl_priv->use_irq==2)) 
		disable_irq(ssl_priv->irq);// free_irq()��ƽ̨ʵ�����ʹ�û�disable
}

static int ssd253x_ts_resume(struct i2c_client *client)
{
	struct ssl_ts_priv *ssl_priv = dev_get_drvdata(&client->dev);
	printk("ssd253x_ts_resume!\n");
	deviceResume(client);
	ssd253x_record = 0;
	ssd253x_current = 0;
	ssd253x_record_times = 0;
	ssd253x_timer_flag = 0;
	if(ssl_priv->use_irq) 
		enable_irq(ssl_priv->irq);
	else 
		hrtimer_start(&ssl_priv->timer, ktime_set(1, 0), HRTIMER_MODE_REL);
	return 0;
}

static int ssd253x_ts_suspend(struct i2c_client *client, pm_message_t mesg)
{
	struct ssl_ts_priv *ssl_priv = dev_get_drvdata(&client->dev);
	printk("ssd253x_ts_suspend!\n");
	deviceSuspend(client);		
	if((ssl_priv->use_irq==0)||(ssl_priv->use_irq==2)) hrtimer_cancel(&ssl_priv->timer);
	if((ssl_priv->use_irq==1)||(ssl_priv->use_irq==2)) 
		disable_irq(ssl_priv->irq);	// free_irq()��ƽ̨ʵ�����ʹ�û�disable
	return 0;
}

#ifdef	CONFIG_HAS_EARLYSUSPEND
static void ssd253x_ts_late_resume(struct early_suspend *h)
{
	struct ssl_ts_priv *ssl_priv = container_of(h, struct ssl_ts_priv, early_suspend);
	#ifdef CONFIG_TOUCHSCREEN_SSL_DEBUG
	printk("ssd253x_ts_late_resume! \n");
	#endif
	ssd253x_ts_resume(ssl_priv->client);
}
static void ssd253x_ts_early_suspend(struct early_suspend *h)
{
	struct ssl_ts_priv *ssl_priv = container_of(h, struct ssl_ts_priv, early_suspend);
	#ifdef CONFIG_TOUCHSCREEN_SSL_DEBUG
	printk("ssd253x_ts_early_suspend!\n");
	#endif
	ssd253x_ts_suspend(ssl_priv->client, PMSG_SUSPEND);
}
#endif

static int ssd253x_ts_remove(struct i2c_client *client)
{
	struct ssl_ts_priv *ssl_priv = dev_get_drvdata(&client->dev);	
	#ifdef CONFIG_TOUCHSCREEN_SSL_DEBUG
	printk("ssd253x_ts_remove !\n");
	#endif	
	if((ssl_priv->use_irq==0)||(ssl_priv->use_irq==2)) hrtimer_cancel(&ssl_priv->timer);
	if((ssl_priv->use_irq==1)||(ssl_priv->use_irq==2)) 
		disable_irq(ssl_priv->irq);// free_irq()��ƽ̨ʵ�����ʹ�û�disable
	input_unregister_device(ssl_priv->input);
	input_free_device(ssl_priv->input);
	kfree(ssl_priv);
	dev_set_drvdata(&client->dev, NULL);
	return 0;
}

static irqreturn_t ssd253x_ts_isr(int irq, void *dev_id)
{
	int reg_val;
	struct ssl_ts_priv *ssl_priv = dev_id;
	#ifdef CONFIG_TOUCHSCREEN_SSL_DEBUG
	printk("ssd253x_ts_isr!\n");
	#endif	
	
	//disable_irq_nosync(ssl_priv->irq);             //
	
	reg_val = readl(gpio_addr + PIO_INT_STAT_OFFSET);
     
	if(reg_val&(1<<(IRQ_EINT21)))
	{	
		//printk("==IRQ_EINT21=\n");
		writel(reg_val&(1<<(IRQ_EINT21)),gpio_addr + PIO_INT_STAT_OFFSET);
		queue_work(ssd253x_wq, &ssl_priv->ssl_work);
	}
	else
	{
	    printk("Other Interrupt\n");
	    return IRQ_NONE;
	}
	return IRQ_HANDLED;	
	
}

static enum hrtimer_restart ssd253x_ts_timer(struct hrtimer *timer)
{
	struct ssl_ts_priv *ssl_priv = container_of(timer, struct ssl_ts_priv, timer);
	#ifdef CONFIG_TOUCHSCREEN_SSL_DEBUG
	printk("ssd253x_ts_timer! \n");
	#endif
	queue_work(ssd253x_wq, &ssl_priv->ssl_work);
	if(ssl_priv->use_irq==0) hrtimer_start(&ssl_priv->timer, ktime_set(0, MicroTimeTInterupt), HRTIMER_MODE_REL);
	return HRTIMER_NORESTART;
}

static const struct i2c_device_id ssd253x_ts_id[] = {
	{ SSD253X_I2C_NAME, 0 },
	{ }
};

MODULE_DEVICE_TABLE(i2c, ssd253x_ts_id);

/************************************************************************/

/**
 * ctp_fetch_sysconfig_para - get config info from sysconfig.fex file.
 * return value:  
 *                    = 0; success;
 *                    < 0; err
                      by jicky add
 */
static int ctp_fetch_sysconfig_para(void)
{
	int ret = -1;
	int ctp_used = -1;
	char name[I2C_NAME_SIZE];
	script_parser_value_type_t type = SCRIPT_PARSER_VALUE_TYPE_STRING;

	printk("%s. \n", __func__);

	if(SCRIPT_PARSER_OK != script_parser_fetch("ctp_para", "ctp_used", &ctp_used, 1)){
		pr_err("%s: script_parser_fetch err1. \n", __func__);
		goto script_parser_fetch_err;
	}
	if(1 != ctp_used){
		pr_err("%s: ctp_unused. \n",  __func__);
		return ret;
	}
    /*
	if(SCRIPT_PARSER_OK != script_parser_fetch_ex("ctp_para", "ctp_name", (int *)(&name), &type, sizeof(name)/sizeof(int))){
		pr_err("%s: script_parser_fetch err2. \n", __func__);
		goto script_parser_fetch_err;
	}
	if(strcmp(CTP_NAME, name)){
		pr_err("%s: name %s does not match CTP_NAME. \n", __func__, name);
		pr_err(CTP_NAME);
		//ret = 1;
		return ret;
	}

	if(SCRIPT_PARSER_OK != script_parser_fetch("ctp_para", "ctp_twi_addr", &twi_addr, sizeof(twi_addr)/sizeof(__u32))){
		pr_err("%s: script_parser_fetch err3. \n", name);
		goto script_parser_fetch_err;
	}
	*/

	if(SCRIPT_PARSER_OK != script_parser_fetch("ctp_para", "ctp_type", &ctp_type, sizeof(ctp_type)/sizeof(__u32))){
		pr_err("ctp_type script_parser_fetch err3. \n", ctp_type);
		ctp_type=0;
	}
	printk("%s: ctp_type is %d. \n",ctp_type);

	//big-endian or small-endian?
	u_i2c_addr.dirty_addr_buf[0] = CTP_IIC_ADDR;//twi_addr;
	u_i2c_addr.dirty_addr_buf[1] = I2C_CLIENT_END;
	printk("%s: after: ctp_twi_addr is 0x%x, dirty_addr_buf: 0x%hx. dirty_addr_buf[1]: 0x%hx \n", __func__, twi_addr, u_i2c_addr.dirty_addr_buf[0], u_i2c_addr.dirty_addr_buf[1]);
	
	if(SCRIPT_PARSER_OK != script_parser_fetch("ctp_para", "ctp_twi_id", &twi_id, sizeof(twi_id)/sizeof(__u32))){
		pr_err("%s: script_parser_fetch err4. \n", name);
		goto script_parser_fetch_err;
	}
	printk("%s: ctp_twi_id is %d. \n", __func__, twi_id);
	
	if(SCRIPT_PARSER_OK != script_parser_fetch("ctp_para", "ctp_screen_max_x", &screen_max_x, 1)){
		pr_err("%s: script_parser_fetch err5. \n", __func__);
		goto script_parser_fetch_err;
	}
	pr_info("%s: screen_max_x = %d. \n", __func__, screen_max_x);

	if(SCRIPT_PARSER_OK != script_parser_fetch("ctp_para", "ctp_screen_max_y", &screen_max_y, 1)){
		pr_err("%s: script_parser_fetch err6. \n", __func__);
		goto script_parser_fetch_err;
	}
	pr_info("%s: screen_max_y = %d. \n", __func__, screen_max_y);

	if(SCRIPT_PARSER_OK != script_parser_fetch("ctp_para", "ctp_revert_x_flag", &revert_x_flag, 1)){
		pr_err("%s: script_parser_fetch err7. \n", __func__);
		goto script_parser_fetch_err;
	}
	pr_info("%s: revert_x_flag = %d. \n", __func__, revert_x_flag);

	if(SCRIPT_PARSER_OK != script_parser_fetch("ctp_para", "ctp_revert_y_flag", &revert_y_flag, 1)){
		pr_err("%s: script_parser_fetch err8. \n", __func__);
		goto script_parser_fetch_err;
	}
	pr_info("%s: revert_y_flag = %d. \n", __func__, revert_y_flag);

	if(SCRIPT_PARSER_OK != script_parser_fetch("ctp_para", "ctp_exchange_x_y_flag", &exchange_x_y_flag, 1)){
		pr_err("ft5x_ts: script_parser_fetch err9. \n");
		goto script_parser_fetch_err;
	}
	pr_info("%s: exchange_x_y_flag = %d. \n", __func__, exchange_x_y_flag);

	return 0;

script_parser_fetch_err:
	pr_notice("=========script_parser_fetch_err0============\n");
	return ret;
}


/**
 * ctp_detect - Device detection callback for automatic device creation
 * return value:  
 *                    = 0; success;
 *                    < 0; err
 */
int ctp_detect(struct i2c_client *client, struct i2c_board_info *info)
{
	struct i2c_adapter *adapter = client->adapter;

	if(twi_id == adapter->nr)
	{
		pr_info("%s: Detected chip %s at adapter %d, address 0x%02x\n",
			 __func__, CTP_NAME, i2c_adapter_id(adapter), client->addr);

		strlcpy(info->type, CTP_NAME, I2C_NAME_SIZE);
		return 0;
	}else{
		return -ENODEV;
	}

}
////////////////////////////////////////////////////////////////

static struct ctp_platform_ops ctp_ops = {
	/*.get_pendown_state = ctp_get_pendown_state,
	.clear_penirq	   = ctp_clear_penirq,
	.set_irq_mode      = ctp_set_irq_mode,
	.set_gpio_mode     = ctp_set_gpio_mode,	
	.judge_int_occur   = ctp_judge_int_occur,
	.init_platform_resource = ctp_init_platform_resource,
	.free_platform_resource = ctp_free_platform_resource,

	.ts_reset =          ctp_reset,
	.ts_wakeup =         ctp_wakeup,*/
	.fetch_sysconfig_para = ctp_fetch_sysconfig_para,
	.ts_detect = ctp_detect,
};

/*******************************************************************/
static struct i2c_driver ssd253x_ts_driver = {
	.driver = {
		.name = SSD253X_I2C_NAME,
	},
	.class = I2C_CLASS_HWMON,
	.probe = ssd253x_ts_probe,
	.remove = ssd253x_ts_remove,
#ifndef	CONFIG_HAS_EARLYSUSPEND
	.suspend = ssd253x_ts_suspend,
	.resume = ssd253x_ts_resume,
#endif
	.id_table = ssd253x_ts_id,
	.address_list	= u_i2c_addr.normal_i2c,
};

static struct i2c_board_info i2c_info_dev =  {
	I2C_BOARD_INFO(SSD253X_I2C_NAME, CTP_IIC_ADDR),
	.platform_data	= NULL,
};

static int add_ctp_device(void) {
	int twi_id = 0;
	struct i2c_adapter *adap;
	script_parser_fetch("ctp_para", "ctp_twi_id", &twi_id, 1);
	adap = i2c_get_adapter(twi_id);
	i2c_new_device(adap, &i2c_info_dev);
	return 0;

script_parser_fetch_err:
	return -1;
}



//static char banner[] __initdata = KERN_INFO "SSL Touchscreen driver, (c) 2011 Solomon Systech Ltd.\n";
static int __init ssd253x_ts_init(void)
{
	int ret = -1;
	printk("=============%s============\n", __func__);
    	printk("Version =%s\n",VERSION);
	
	if(add_ctp_device()<0)
	    return -1;
	    
			
	if (ctp_ops.fetch_sysconfig_para())
	{
		printk("%s: err.\n", __func__);
		return -1;
	} 
	
        ssd253x_ts_driver.detect = ctp_ops.ts_detect;
   
	if(i2c_add_driver(&ssd253x_ts_driver)) 
        {
         	printk("ssd253x_ts_init: i2c_add_driver Error! \n");
         	return -1;
        }
    
	return 0; 	
}

static void __exit ssd253x_ts_exit(void)
{
	printk("ssd253x_ts_exit!\n");
	i2c_del_driver(&ssd253x_ts_driver);
	if (ssd253x_wq) destroy_workqueue(ssd253x_wq);
}

module_init(ssd253x_ts_init);
module_exit(ssd253x_ts_exit);

MODULE_AUTHOR("Solomon Systech Ltd - Design Technology, Icarus Choi");
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("ssd253x Touchscreen Driver 1.3");
