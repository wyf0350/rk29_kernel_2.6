/* arch/arm/mach-rk2818/gpio.c
 *
 * Copyright (C) 2010 ROCKCHIP, Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
#include <linux/clk.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/io.h>
#include <linux/sysdev.h>

#include <mach/hardware.h>
#include <mach/gpio.h>
#include <mach/rk2818_iomap.h>
#include <mach/iomux.h>

#include <asm/gpio.h>

#define to_rk2818_gpio_chip(c) container_of(c, struct rk2818_gpio_chip, chip)

struct rk2818_gpio_chip {
	struct gpio_chip	chip;				/*RK2818���GPIO�����������߼���Ϣ*/
	struct rk2818_gpio_chip	*next;		/* Bank sharing same clock */
	struct rk2818_gpio_bank	*bank;		/* Bank definition */
	unsigned char  __iomem	*regbase;	/* Base of register bank */
};

static int gpio_banks;//GPIO����
static int gpio_banksInGrp;//num of GPIOS.eg:GPIO0_X or GPIO1_X(X=A\B\C\D)
/*
 * This lock class tells lockdep that GPIO irqs are in a different
 * category than their parents, so it won't report false recursion.
 */
static struct lock_class_key gpio_lock_class;

static void rk2818_gpiolib_dbg_show(struct seq_file *s, struct gpio_chip *chip);
static void rk2818_gpiolib_set(struct gpio_chip *chip, unsigned offset, int val);
static int rk2818_gpiolib_get(struct gpio_chip *chip, unsigned offset);
static int rk2818_gpiolib_direction_output(struct gpio_chip *chip,
					 unsigned offset, int val);
static int rk2818_gpiolib_direction_input(struct gpio_chip *chip,
					unsigned offset);
static int rk2818_gpiolib_PullUpDown(struct gpio_chip *chip, unsigned offset, unsigned value);
static int rk2818_gpiolib_to_irq(struct gpio_chip *chip,
						unsigned offset);
/*
�����˽ṹ��rk2818gpio_chip���飬��ע����GPIO��ز���
�Ļص�����������߼���Ϣ��
*/
#define RK2818_GPIO_CHIP(name, base_gpio, nr_gpio)			\
	{								\
		.chip = {						\
			.label		  = name,			\
			.direction_input  = rk2818_gpiolib_direction_input, \
			.direction_output = rk2818_gpiolib_direction_output, \
			.get		  = rk2818_gpiolib_get,		\
			.set		  = rk2818_gpiolib_set,		\
			.pull_updown  = rk2818_gpiolib_PullUpDown,         \
			.dbg_show	  = rk2818_gpiolib_dbg_show,	\
			.to_irq       = rk2818_gpiolib_to_irq,     \
			.base		  = base_gpio,			\
			.ngpio		  = nr_gpio,			\
		},							\
	}

static struct rk2818_gpio_chip rk2818gpio_chip[] = {
	RK2818_GPIO_CHIP("A", 0*NUM_GROUP + PIN_BASE, NUM_GROUP),
	RK2818_GPIO_CHIP("B", 1*NUM_GROUP + PIN_BASE, NUM_GROUP),
	RK2818_GPIO_CHIP("C", 2*NUM_GROUP + PIN_BASE, NUM_GROUP),
	RK2818_GPIO_CHIP("D", 3*NUM_GROUP + PIN_BASE, NUM_GROUP),
	RK2818_GPIO_CHIP("A", 4*NUM_GROUP + PIN_BASE, NUM_GROUP),
	RK2818_GPIO_CHIP("B", 5*NUM_GROUP + PIN_BASE, NUM_GROUP),
	RK2818_GPIO_CHIP("C", 6*NUM_GROUP + PIN_BASE, NUM_GROUP),
	RK2818_GPIO_CHIP("D", 7*NUM_GROUP + PIN_BASE, NUM_GROUP),
};
static u32 wakeups[MAX_GPIO_BANKS];
static u32 wakeupsDepth[MAX_GPIO_BANKS];
/*----------------------------------------------------------------------
Name	: rk2818_gpio_write
Desc		: ��ָ���Ĵ���д��ָ��ֵ
Params	: �������:
				regbase	:ָ���Ĵ����Ļ���ַ
				regOff	:ָ���Ĵ�����ƫ�Ƶ�ַ
				val		:ָ��д�����ֵ			
		  �������:
				��
Return	:
		 ��
----------------------------------------------------------------------*/
static inline void rk2818_gpio_write(unsigned char  __iomem	*regbase, unsigned int regOff,unsigned int val)
{
	__raw_writel(val,regbase + regOff);
	return ;
}

/*----------------------------------------------------------------------
Name	: rk2818_gpio_read
Desc		: ��ȡָ���Ĵ�������ֵ
Params	: �������:
				regbase	:ָ���Ĵ����Ļ���ַ
				regOff	:ָ���Ĵ�����ƫ�Ƶ�ַ			
		  �������:
				��
Return	:
		��ȡ������ֵ
----------------------------------------------------------------------*/
static inline unsigned int rk2818_gpio_read(unsigned char  __iomem	*regbase, unsigned int regOff)
{
	return __raw_readl(regbase + regOff);
}

/*----------------------------------------------------------------------
Name	: rk2818_gpio_bitOp
Desc		: ��ָ���Ĵ��������߼�����
Params	: �������:
				regbase	:ָ���Ĵ����Ļ���ַ
				regOff	:ָ���Ĵ�����ƫ�Ƶ�ַ
				mask	:����λ
				opFlag	:0--������0����;1--���л�1����
		  �������:
				��
Return	:
		 ��
----------------------------------------------------------------------*/
static inline void rk2818_gpio_bitOp(unsigned char  __iomem	*regbase, unsigned int regOff,unsigned int mask,unsigned char opFlag)
{
	unsigned int valTemp = 0;
	unsigned char  __iomem	*regAddr = regbase + regOff;
	
	if(opFlag == 0)//�ԼĴ�����Ӧλ������0����
	{
		valTemp = __raw_readl(regAddr);
		valTemp &= (~mask);
		//printk(KERN_INFO "rk2818_gpio_bitOp--0:regAddr=%x,valTemp=%x\n",regAddr,valTemp);
		__raw_writel(valTemp,regAddr);
	}
	else if(opFlag == 1)//�ԼĴ�����Ӧλ���л�1����
	{
		valTemp = __raw_readl(regAddr);
		valTemp |= mask;
		//printk(KERN_INFO "rk2818_gpio_bitOp--1:regAddr=%x,valTemp=%x\n",regAddr,valTemp);
		__raw_writel(valTemp,regAddr);
	}

	return;
}

/*----------------------------------------------------------------------
Name	: GPIOSetPinDirection
Desc		: ���ö˿ڷ���PA��PEĬ��ΪDebounce��ʽ
Params	: �������:
				chip		:rk2818������chip����&rk2818_gpio->chip
				mask	:�Ĵ�������λ
				direction	:�����������	 0--����; 1--���				
		  �������:
				��
Return	:
		  0	--- �ɹ�
		  -1	--- ʧ��
----------------------------------------------------------------------*/
static int GPIOSetPinDirection(struct gpio_chip *chip, unsigned int mask,eGPIOPinDirection_t direction)
{
	struct rk2818_gpio_chip *rk2818_gpio = to_rk2818_gpio_chip(chip);
	unsigned char  __iomem	*gpioRegBase = rk2818_gpio->regbase;

	if(!rk2818_gpio || !gpioRegBase)
	{
		return -1;
	}
	
	if(strcmp(rk2818_gpio->chip.label,"A") == 0)
	{
		rk2818_gpio_bitOp(gpioRegBase,GPIO_SWPORTA_DDR,mask,direction);
		rk2818_gpio_bitOp(gpioRegBase,GPIO_DEBOUNCE,mask,1); 
	}
	else	if(strcmp(rk2818_gpio->chip.label,"B") == 0)
	{
		rk2818_gpio_bitOp(gpioRegBase,GPIO_SWPORTB_DDR,mask,direction);
	}
	else	if(strcmp(rk2818_gpio->chip.label,"C") == 0)
	{
		rk2818_gpio_bitOp(gpioRegBase,GPIO_SWPORTC_DDR,mask,direction);
	}
	else	if(strcmp(rk2818_gpio->chip.label,"D") == 0)
	{
		rk2818_gpio_bitOp(gpioRegBase,GPIO_SWPORTD_DDR,mask,direction);
	}
	else
	{
		return -1;
	}
	return 0;

}

/*----------------------------------------------------------------------
Name	: GPIOSetPinLevel
Desc		: ���ö˿ڵ�ƽ
Params	: �������:
				chip		:rk2818������chip����&rk2818_gpio->chip
				mask	:�Ĵ�������λ
				level		:��ƽ	 0:low 1:high					
		  �������:
				��
Return	:
		  0	--- �ɹ�
		  -1	--- ʧ��
----------------------------------------------------------------------*/
static int GPIOSetPinLevel(struct gpio_chip *chip, unsigned int mask,eGPIOPinLevel_t level)
{
	struct rk2818_gpio_chip *rk2818_gpio = to_rk2818_gpio_chip(chip);
	unsigned char  __iomem	*gpioRegBase = rk2818_gpio->regbase;

	if(!rk2818_gpio || !gpioRegBase)
	{
		return -1;
	}
	
	if(strcmp(rk2818_gpio->chip.label,"A") == 0)
	{
		rk2818_gpio_bitOp(gpioRegBase,GPIO_SWPORTA_DDR,mask,1);
		rk2818_gpio_bitOp(gpioRegBase,GPIO_SWPORTA_DR,mask,level);
	}
	else	if(strcmp(rk2818_gpio->chip.label,"B") == 0)
	{
		rk2818_gpio_bitOp(gpioRegBase,GPIO_SWPORTB_DDR,mask,1);
		rk2818_gpio_bitOp(gpioRegBase,GPIO_SWPORTB_DR,mask,level);
	}
	else	if(strcmp(rk2818_gpio->chip.label,"C") == 0)
	{
		rk2818_gpio_bitOp(gpioRegBase,GPIO_SWPORTC_DDR,mask,1);
		rk2818_gpio_bitOp(gpioRegBase,GPIO_SWPORTC_DR,mask,level);
	}
	else	if(strcmp(rk2818_gpio->chip.label,"D") == 0)
	{
		rk2818_gpio_bitOp(gpioRegBase,GPIO_SWPORTD_DDR,mask,1);
		rk2818_gpio_bitOp(gpioRegBase,GPIO_SWPORTD_DR,mask,level);
	}
	else
	{
		return -1;
	}
	return 0;
}

/*----------------------------------------------------------------------
Name	: GPIOGetPinLevel
Desc		: ��ȡ�˿ڵ�ƽ
Params	: �������:
				chip		:rk2818������chip����&rk2818_gpio->chip
				mask	:�Ĵ�������λ				
		  �������:
				��
Return	:
		  0	--- �͵�ƽ
		  1	--- �ߵ�ƽ
		  -1	--- ʧ��
----------------------------------------------------------------------*/
static int GPIOGetPinLevel(struct gpio_chip *chip, unsigned int mask)
{
	unsigned int valTemp;
	struct rk2818_gpio_chip *rk2818_gpio = to_rk2818_gpio_chip(chip);
	unsigned char  __iomem	*gpioRegBase = rk2818_gpio->regbase;

	if(!rk2818_gpio || !gpioRegBase)
	{
		return -1;
	}
	
	if(strcmp(rk2818_gpio->chip.label,"A") == 0)
	{
	    	valTemp = rk2818_gpio_read(gpioRegBase,GPIO_EXT_PORTA);
	}
	else	if(strcmp(rk2818_gpio->chip.label,"B") == 0)
	{
		valTemp = rk2818_gpio_read(gpioRegBase,GPIO_EXT_PORTB);
	}
	else	if(strcmp(rk2818_gpio->chip.label,"C") == 0)
	{
		valTemp = rk2818_gpio_read(gpioRegBase,GPIO_EXT_PORTC);
	}
	else	if(strcmp(rk2818_gpio->chip.label,"D") == 0)
	{
		valTemp = rk2818_gpio_read(gpioRegBase,GPIO_EXT_PORTD);
	}
	else
	{
		return -1;
	}
//printk(KERN_INFO "GPIOGetPinLevel:%s,%x,%x,%x\n",rk2818_gpio->chip.label,rk2818_gpio->regbase,mask,valTemp);
	return ((valTemp & mask) != 0);
}

/*----------------------------------------------------------------------
Name	: GPIOEnableIntr
Desc		: ���ö˿ڵ��жϽţ�ֻ��PA��PE�ڲſ����ж�
Params	: �������:
				chip		:rk2818������chip����&rk2818_gpio->chip
				mask	:�Ĵ�������λ				
		  �������:
				��
Return	:
		  0	--- �ɹ�
		  -1	--- ʧ��
----------------------------------------------------------------------*/
static int GPIOEnableIntr(struct gpio_chip *chip, unsigned int mask)
{
	struct rk2818_gpio_chip *rk2818_gpio = to_rk2818_gpio_chip(chip);
	unsigned char  __iomem	*gpioRegBase = rk2818_gpio->regbase;

	if(!rk2818_gpio || !gpioRegBase)
	{
		return -1;
	}
	
	rk2818_gpio_bitOp(gpioRegBase,GPIO_INTEN,mask,1);
	return 0;
}

/*----------------------------------------------------------------------
Name	: GPIODisableIntr
Desc		:  ���ö˿ڲ����жϣ�ֻ��PA��PE�ſ����ж�
Params	: �������:
				chip		:rk2818������chip����&rk2818_gpio->chip
				mask	:�Ĵ�������λ			
		  �������:
				��
Return	:
		  0	--- �ɹ�
		  -1	--- ʧ��
----------------------------------------------------------------------*/
static int GPIODisableIntr(struct gpio_chip *chip, unsigned int mask)
{
	struct rk2818_gpio_chip *rk2818_gpio = to_rk2818_gpio_chip(chip);
	unsigned char  __iomem	*gpioRegBase = rk2818_gpio->regbase;

	if(!rk2818_gpio || !gpioRegBase)
	{
		return -1;
	}
	
	rk2818_gpio_bitOp(gpioRegBase,GPIO_INTEN,mask,0);
	return 0;
}
#if 0
/*----------------------------------------------------------------------
Name	: GPIOClearIntr
Desc		: ����жϿڱ�־��ֻ���ڱ����ж�ʱ��Ч����ƽ�ж���Ч
Params	: �������:
				chip		:rk2818������chip����&rk2818_gpio->chip
				mask	:�Ĵ�������λ			
		  �������:
				��
Return	:
		  0	--- �ɹ�
		  -1	--- ʧ��
----------------------------------------------------------------------*/
static int GPIOClearIntr(struct gpio_chip *chip, unsigned int mask)
{	 
	struct rk2818_gpio_chip *rk2818_gpio = to_rk2818_gpio_chip(chip);
	unsigned char  __iomem	*gpioRegBase = rk2818_gpio->regbase;

	if(!rk2818_gpio || !gpioRegBase)
	{
		return -1;
	}
	
	rk2818_gpio_bitOp(gpioRegBase,GPIO_PORTS_EOI,mask,1);
	return  0;
}
#endif
/*----------------------------------------------------------------------
Name	: GPIOInmarkIntr
Desc		: ���������е�ĳ���жϣ��ڵ�ƽ�ж��б���Ҫ�����������������ж�
Params	: �������:
				chip		:rk2818������chip����&rk2818_gpio->chip
				mask	:�Ĵ�������λ				
		  �������:
				��
Return	:
		  0	--- �ɹ�
		  -1	--- ʧ��
----------------------------------------------------------------------*/
static int GPIOInmarkIntr(struct gpio_chip *chip, unsigned int mask)
{
	struct rk2818_gpio_chip *rk2818_gpio = to_rk2818_gpio_chip(chip);
	unsigned char  __iomem	*gpioRegBase = rk2818_gpio->regbase;

	if(!rk2818_gpio || !gpioRegBase)
	{
		return -1;
	}
	
	rk2818_gpio_bitOp(gpioRegBase,GPIO_INTMASK,mask,1);
	return 0;
}

/*----------------------------------------------------------------------
Name	: GPIOUnInmarkIntr
Desc		: ������ĳ���жϣ���Ե��ǵ�ƽ�ж�
Params	: �������:
				chip		:rk2818������chip����&rk2818_gpio->chip
				mask	:�Ĵ�������λ				
		  �������:
				��
Return	:
		  0	--- �ɹ�
		  -1	--- ʧ��
----------------------------------------------------------------------*/
static int GPIOUnInmarkIntr(struct gpio_chip *chip, unsigned int mask)
{
	struct rk2818_gpio_chip *rk2818_gpio = to_rk2818_gpio_chip(chip);
	unsigned char  __iomem	*gpioRegBase = rk2818_gpio->regbase;

	if(!rk2818_gpio || !gpioRegBase)
	{
		return -1;
	}
	
	rk2818_gpio_bitOp(gpioRegBase,GPIO_INTMASK,mask,0);
	return 0;
}

/*----------------------------------------------------------------------
Name	: GPIOSetIntrType
Desc		: �����ж�����
Params	: �������:
				chip		:rk2818������chip����&rk2818_gpio->chip
				mask	:�Ĵ�������λ
				IntType	:�жϴ�������			
		  �������:
				��
Return	:
		  0	--- �ɹ�
		  -1	--- ʧ��
----------------------------------------------------------------------*/
static int GPIOSetIntrType(struct gpio_chip *chip, unsigned int mask, eGPIOIntType_t IntType)
{
	struct rk2818_gpio_chip *rk2818_gpio = to_rk2818_gpio_chip(chip);
	unsigned char  __iomem	*gpioRegBase = rk2818_gpio->regbase;

	
	if(!rk2818_gpio || !gpioRegBase)
	{
		return -1;
	}
	
	switch ( IntType )
	{
	        case GPIOLevelLow:
			rk2818_gpio_bitOp(gpioRegBase,GPIO_INT_POLARITY,mask,0);	
			rk2818_gpio_bitOp(gpioRegBase,GPIO_INTTYPE_LEVEL,mask,0);	
			break;
	        case GPIOLevelHigh:
			rk2818_gpio_bitOp(gpioRegBase,GPIO_INTTYPE_LEVEL,mask,0);	
			rk2818_gpio_bitOp(gpioRegBase,GPIO_INT_POLARITY,mask,1);	
			break;
	        case GPIOEdgelFalling:
			rk2818_gpio_bitOp(gpioRegBase,GPIO_INTTYPE_LEVEL,mask,1);	
			rk2818_gpio_bitOp(gpioRegBase,GPIO_INT_POLARITY,mask,0);	
			break;
	        case GPIOEdgelRising:
			rk2818_gpio_bitOp(gpioRegBase,GPIO_INTTYPE_LEVEL,mask,1);	
			rk2818_gpio_bitOp(gpioRegBase,GPIO_INT_POLARITY,mask,1);	
			break;
		default:
			return(-1);
	}
	 return(0);
}

/*----------------------------------------------------------------------
Name	: GPIOPullUpDown
Desc		: �����еĽ�λ���ж˿�������������ʼ��
Params	: �������:
				chip		:rk2818������chip����&rk2818_gpio->chip
				offset	:ƫ��ֵ
				GPIOPullUpDown	:0--	����;1--����;2--����;3--δ��ʼ��		
		  �������:
				��
Return	:
		  0	--- �ɹ�
		  -1	--- ʧ��
----------------------------------------------------------------------*/
static int GPIOPullUpDown(struct gpio_chip *chip, unsigned int offset, eGPIOPullType_t GPIOPullUpDown)
{
	unsigned char temp=0;
	struct rk2818_gpio_chip *rk2818_gpio = to_rk2818_gpio_chip(chip);
	unsigned char  __iomem *pAPBRegBase = (unsigned char  __iomem *)RK2818_REGFILE_BASE;
 	unsigned int mask1 = 0,mask2 = 0;

	if(!rk2818_gpio || !pAPBRegBase)
	{
		return -1;
	}
	
	if(offset >= 8)
	{
		return -1;
	}

	if(rk2818_gpio->bank->id%2 == 1)
	{
		temp = 16;
	}
	mask1 = 0x03<<(2*offset+temp);
	mask2 = GPIOPullUpDown <<(2*offset+temp);
	if(rk2818_gpio->bank->id==RK2818_ID_PIOA || rk2818_gpio->bank->id==RK2818_ID_PIOB)
	{
		
		rk2818_gpio_bitOp(pAPBRegBase,GPIO0_AB_PU_CON,mask1,0);
		rk2818_gpio_bitOp(pAPBRegBase,GPIO0_AB_PU_CON,mask2,1);
	}
	else if(rk2818_gpio->bank->id==RK2818_ID_PIOC || rk2818_gpio->bank->id==RK2818_ID_PIOD)
	{
		rk2818_gpio_bitOp(pAPBRegBase,GPIO0_CD_PU_CON,mask1,0);
		rk2818_gpio_bitOp(pAPBRegBase,GPIO0_CD_PU_CON,mask2,1);
	}
	else if(rk2818_gpio->bank->id==RK2818_ID_PIOE || rk2818_gpio->bank->id==RK2818_ID_PIOF)
	{
		rk2818_gpio_bitOp(pAPBRegBase,GPIO1_AB_PU_CON,mask1,0);
		rk2818_gpio_bitOp(pAPBRegBase,GPIO1_AB_PU_CON,mask2,1);
	}
	else if(rk2818_gpio->bank->id==RK2818_ID_PIOG|| rk2818_gpio->bank->id==RK2818_ID_PIOH)
	{
		rk2818_gpio_bitOp(pAPBRegBase,GPIO1_CD_PU_CON,mask1,0);
		rk2818_gpio_bitOp(pAPBRegBase,GPIO1_CD_PU_CON,mask2,1);
	}
	else
	{
		return -1;
	}
	return 0;
}

/*----------------------------------------------------------------------
Name	: pin_to_gpioChip
Desc		: ���������PINֵ�����Ӧ��chip�ṹ���׵�ַ
Params	: �������:
				pin		:�����PINֵ				
		  �������:
				��
Return	:
		 ���ض�Ӧ��chip�ṹ���׵�ַ��ʧ���򷵻�NULL
----------------------------------------------------------------------*/
static inline  struct gpio_chip *pin_to_gpioChip(unsigned pin)
{
	if(pin < PIN_BASE)
		return NULL;
	
	pin -= PIN_BASE;
	pin /= NUM_GROUP;
	if (likely(pin < gpio_banks))
		return &(rk2818gpio_chip[pin].chip);
	return NULL;
}

/*----------------------------------------------------------------------
Name	: pin_to_mask
Desc		: ���������PINֵ�����Ӧ������λ
Params	: �������:
				pin		:�����PINֵ	
		  �������:
				��
Return	:
		  ���ض�Ӧ������λֵ��0��ʾʧ��
----------------------------------------------------------------------*/
static inline unsigned  pin_to_mask(unsigned pin)
{
	if(pin < PIN_BASE)
		return 0;
	pin -= PIN_BASE;
	return 1ul << (pin % NUM_GROUP);
}

/*----------------------------------------------------------------------
Name	: offset_to_mask
Desc		: ����ƫ���������Ӧ������λֵ
Params	: �������:
				offset		:ƫ����				
		  �������:
				��
Return	:
		 ���ض�Ӧ������λֵ
----------------------------------------------------------------------*/
static inline unsigned  offset_to_mask(unsigned offset)
{
	return 1ul << (offset% NUM_GROUP);
}
static int gpio_irq_set_wake(unsigned irq, unsigned state)
{	
	unsigned int pin = irq_to_gpio(irq);
	unsigned	mask = pin_to_mask(pin);
	unsigned	bank = (pin - PIN_BASE) / NUM_GROUP;

	if (unlikely(bank >= MAX_GPIO_BANKS))
		return -EINVAL;
	
	if(rk2818gpio_chip[bank].bank->id == RK2818_ID_PIOA)
	{
		set_irq_wake(IRQ_NR_GPIO0, state);
	}
	else if(rk2818gpio_chip[bank].bank->id == RK2818_ID_PIOE)
	{
		set_irq_wake(IRQ_NR_GPIO1, state);
	}
	else
	{
		return 0;
	}
	
	if (state)
		wakeups[bank] |= mask;
	else
		wakeups[bank] &= ~mask;
	
	return 0;

}

static int rk2818_gpio_resume(struct sys_device *dev)
{
	int i;
	
	for (i = 0; i < gpio_banks; i+=gpio_banksInGrp) {
		printk("rk2818_gpio_resume:wakeups[%d]=%d,wakeupsDepth[%d]=%d\n",
			i,wakeups[i],i,wakeupsDepth[i]);
		if (!wakeups[i] && wakeupsDepth[i])
		{
			wakeupsDepth[i] = 0;
			clk_enable(rk2818gpio_chip[i].bank->clock);
		}
	}

	return 0;
}

static int rk2818_gpio_suspend(struct sys_device *dev, pm_message_t mesg)
{
	int i;

	for (i = 0; i < gpio_banks; i+=gpio_banksInGrp)  {
		printk("rk2818_gpio_suspend:wakeups[%d]=%d,wakeupsDepth[%d]=%d\n",
			i,wakeups[i],i,wakeupsDepth[i]);
		if (!wakeups[i] && !wakeupsDepth[i])
		{
			wakeupsDepth[i] = 1;
			clk_disable(rk2818gpio_chip[i].bank->clock);
		}
		else if(wakeups[i])
			rk2818_gpio_write(rk2818gpio_chip[i].regbase,GPIO_INTEN,wakeups[i]);
	}

	return 0;
}

 #if 0
int  rk2818_set_gpio_input(unsigned pin, int use_pullup)
{
	struct gpio_chip *chip = pin_to_gpioChip(pin);
	unsigned	mask = pin_to_mask(pin);
	unsigned offset = 0;
	
	if (!chip || !mask)
		return -EINVAL;
	
	offset = (pin-PIN_BASE) % NUM_GROUP;

	if(GPIOSetPinDirection(chip,mask,GPIO_IN) == 0)
	{
		if(use_pullup)
		{
			return GPIOPullUpDown(chip,offset,GPIOPullUp);
		}
		else
		{
			return GPIOPullUpDown(chip,offset,GPIOPullDown);
		}
	}
	else
	{	
		return -1;
	}
}

/*
 * mux the pin to the gpio controller (instead of "A" or "B" peripheral),
 * and configure it for an output.
 */
int  rk2818_set_gpio_output(unsigned pin, int value)
{
	struct gpio_chip *chip = pin_to_gpioChip(pin);
	unsigned	mask = pin_to_mask(pin);

	if (!chip || !mask)
		return -EINVAL;
	
	if(GPIOSetPinDirection(chip,mask,GPIO_OUT) == 0)
	{
		return GPIOSetPinLevel(chip,mask,value);
	}
	else
	{
		return -1;
	}
}

/*
 * assuming the pin is muxed as a gpio output, set its value.
 */
int rk2818_set_gpio_value(unsigned pin, int value)
{
	struct gpio_chip *chip = pin_to_gpioChip(pin);
	unsigned	mask = pin_to_mask(pin);

	if (!chip || !mask)
		return -EINVAL;
	
	GPIOSetPinLevel(chip,mask,value);
	
	return 0;
}

/*
 * read the pin's value (works even if it's not muxed as a gpio).
 */
int rk2818_get_gpio_value(unsigned pin)
{
	struct gpio_chip *chip = pin_to_gpioChip(pin);
	unsigned	mask = pin_to_mask(pin);

	if (!chip || !mask)
		return -EINVAL;
	
	return GPIOGetPinLevel(chip,mask);
}
#endif
/*--------------------------------------------------------------------------*/


 /*----------------------------------------------------------------------
Name	: gpio_irq_disable
Desc		: ��ʹ���жϣ�ֻ��PA��PE�ſ����ж�
Params	: �������:
				irq		:ָ����ʹ�ܵ�IRQ				
		  �������:
				��
Return	:
		  ��
----------------------------------------------------------------------*/
 static void gpio_irq_disable(unsigned irq)
{
	unsigned int pin = irq_to_gpio(irq);
	struct gpio_chip *chip = pin_to_gpioChip(pin);
	unsigned	mask = pin_to_mask(pin);

	if(chip && mask)
		GPIODisableIntr(chip,mask);
	
	return;
}

/*----------------------------------------------------------------------
Name	: gpio_irq_enable
Desc		: ʹ���жϣ�ֻ��PA��PE�ſ����ж�
Params	: �������:
				irq		:ָ��ʹ�ܵ�IRQ								
		  �������:
				��
Return	:
		  ��
----------------------------------------------------------------------*/
static void gpio_irq_enable(unsigned irq)
{
	unsigned int pin = irq_to_gpio(irq);
	struct gpio_chip *chip = pin_to_gpioChip(pin);
	unsigned	mask = pin_to_mask(pin);

	if(chip && mask)
		GPIOEnableIntr(chip,mask);
	
	return;
}

/*----------------------------------------------------------------------
Name	: gpio_irq_mask
Desc		: �����ж�
Params	: �������:
				irq		:ָ�����ε�IRQ								
		  �������:
				��
Return	:
		  ��
----------------------------------------------------------------------*/
static void gpio_irq_mask(unsigned irq)
{
	unsigned int pin = irq_to_gpio(irq);
	struct gpio_chip *chip = pin_to_gpioChip(pin);
	unsigned	mask = pin_to_mask(pin);

	if(chip && mask)
		GPIOInmarkIntr(chip,mask);
}

/*----------------------------------------------------------------------
Name	: gpio_irq_unmask
Desc		: �������ж�
Params	: �������:
				irq		:ָ�������ε�IRQ								
		  �������:
				��
Return	:
		  ��
----------------------------------------------------------------------*/
static void gpio_irq_unmask(unsigned irq)
{
	unsigned int pin = irq_to_gpio(irq);
	struct gpio_chip *chip = pin_to_gpioChip(pin);
	unsigned	mask = pin_to_mask(pin);

	if (chip && mask)
		GPIOUnInmarkIntr(chip,mask);
}
/*----------------------------------------------------------------------
Name	: gpio_irq_type
Desc		: �����ж�����
Params	: �������:
				irq		:ָ����IRQ	
				type		:ָ�����ж�����		
		  �������:
				��
Return	:
		  0	--- �ɹ�
		  -22	--- ʧ��
----------------------------------------------------------------------*/
static int gpio_irq_type(unsigned irq, unsigned type)
{
	unsigned int pin = irq_to_gpio(irq);
	struct gpio_chip *chip = pin_to_gpioChip(pin);
	unsigned	mask = pin_to_mask(pin);
	
	if(!chip || !mask)
		return -EINVAL;
	//����Ϊ�ж�֮ǰ������������Ϊ����״̬
	GPIOSetPinDirection(chip,mask,GPIO_IN);
	
	switch (type) {
		case IRQ_TYPE_NONE:
			break;
		case IRQ_TYPE_EDGE_RISING:
			GPIOSetIntrType(chip,mask,GPIOEdgelRising);
			break;
		case IRQ_TYPE_EDGE_FALLING:
			GPIOSetIntrType(chip,mask,GPIOEdgelFalling);
			break;
		case IRQ_TYPE_EDGE_BOTH:
			break;
		case IRQ_TYPE_LEVEL_HIGH:
			GPIOSetIntrType(chip,mask,GPIOLevelHigh);
			break;
		case IRQ_TYPE_LEVEL_LOW:
			GPIOSetIntrType(chip,mask,GPIOLevelLow);
			break;
		default:
			return -EINVAL;
	}
	return 0;
}

static struct irq_chip rk2818gpio_irqchip = {
	.name		= "RK2818_GPIOIRQ",
	.enable 		= gpio_irq_enable,
	.disable		= gpio_irq_disable,
	.mask		= gpio_irq_mask,
	.unmask		= gpio_irq_unmask,
	.set_type		= gpio_irq_type,
	.set_wake	= gpio_irq_set_wake,
};

/*----------------------------------------------------------------------
Name	: gpio_irq_handler
Desc		: �ж�6���ж�7�Ĵ�������
Params	: �������:
				irq		:���������жϺ�6���жϺ�7
				desc		:�жϽṹ��			
		  �������:
				��
Return	:
		  ��
----------------------------------------------------------------------*/
static void gpio_irq_handler(unsigned irq, struct irq_desc *desc)
{
	unsigned	pin,gpioToirq=0;
	struct irq_desc	*gpio;
	struct rk2818_gpio_chip *rk2818_gpio;
	unsigned char  __iomem	*gpioRegBase;
	u32		isr;

	rk2818_gpio = get_irq_chip_data(irq);
	gpioRegBase = rk2818_gpio->regbase;

	//�����ж�6��7
	desc->chip->mask(irq);
	//��ȡ��ǰ�ж�״̬������ѯ������GPIO���ĸ�PIN������ж�
	isr = rk2818_gpio_read(gpioRegBase,GPIO_INT_STATUS);
	if (!isr) {
			desc->chip->unmask(irq);
			return;
	}

	pin = rk2818_gpio->chip.base;
	gpioToirq = gpio_to_irq(pin);
	gpio = &irq_desc[gpioToirq];

	while (isr) {
		if (isr & 1) {
			//if (unlikely(gpio->depth)) {
				/*
				 * The core ARM interrupt handler lazily disables IRQs so
				 * another IRQ must be generated before it actually gets
				 * here to be disabled on the GPIO controller.
				 */
			//	gpio_irq_mask(gpioToirq);
			//}
			//else
			{
				unsigned int gpio_Int_Level = 0;
				unsigned int mask = pin_to_mask(pin);
				if(!mask)
					break;
				gpio_Int_Level =  rk2818_gpio_read(gpioRegBase,GPIO_INTTYPE_LEVEL);
				if(gpio_Int_Level == 0)//��ʾ���ж������ǵ�ƽ�ж�
				{
					rk2818_gpio_bitOp(gpioRegBase,GPIO_INTMASK,mask,1);
				}
				generic_handle_irq(gpioToirq);
				
				if(gpio_Int_Level)//��ʾ���ж������Ǳ����ж�
				{
					rk2818_gpio_bitOp(gpioRegBase,GPIO_PORTS_EOI,mask,1);
				}
				else//��ʾ���ж������ǵ�ƽ�ж�
				{
					rk2818_gpio_bitOp(gpioRegBase,GPIO_INTMASK,mask,0);
				}
			}
				
		}
		pin++;
		gpio++;
		isr >>= 1;
		gpioToirq = gpio_to_irq(pin);
	}

	desc->chip->unmask(irq);
	/* now it may re-trigger */
}

 /*----------------------------------------------------------------------
Name	: rk2818_gpio_irq_setup
Desc		: enable GPIO interrupt support.
Params	: �������:
				��			
		  �������:
				��
Return	:
		  ��
----------------------------------------------------------------------*/
void __init rk2818_gpio_irq_setup(void)
{
	unsigned	int	i,j, pin,irq=IRQ_NR_GPIO0;
	struct rk2818_gpio_chip *this;
	
	this = rk2818gpio_chip;
	pin = NR_AIC_IRQS;

	for(i=0;i<2;i++)
	{
		rk2818_gpio_write(this->regbase,GPIO_INTEN,0);
		for (j = 0; j < 8; j++) 
		{
			lockdep_set_class(&irq_desc[pin+j].lock, &gpio_lock_class);
			/*
			 * Can use the "simple" and not "edge" handler since it's
			 * shorter, and the AIC handles interrupts sanely.
			 */
			set_irq_chip(pin+j, &rk2818gpio_irqchip);
			set_irq_handler(pin+j, handle_simple_irq);
			set_irq_flags(pin+j, IRQF_VALID);
		}
		if(this->bank->id == RK2818_ID_PIOA)
		{	
			irq = IRQ_NR_GPIO0;
			
		}
		else if(this->bank->id == RK2818_ID_PIOE)
		{
			irq = IRQ_NR_GPIO1;
		}
		set_irq_chip_data(irq, this);
		set_irq_chained_handler(irq, gpio_irq_handler);
		this += 4; 
		pin += 8;
	}
	pr_info("rk2818_gpio_irq_setup: %d gpio irqs in %d banks\n", pin - PIN_BASE, gpio_banks);

}

/*----------------------------------------------------------------------
Name	: rk2818_gpiolib_direction_input
Desc		: ���ö˿�Ϊ����
Params	: �������:
				chip		:rk2818������chip����&rk2818_gpio->chip
				offset	:�Ĵ���λƫ����				
		  �������:
				��
Return	:
		  0	--- �ɹ�
		  -1	--- ʧ��
----------------------------------------------------------------------*/
static int rk2818_gpiolib_direction_input(struct gpio_chip *chip,
					unsigned offset)
{
	unsigned	mask = offset_to_mask(offset);
	
	return GPIOSetPinDirection(chip,mask,GPIO_IN);
}
/*----------------------------------------------------------------------
Name	: rk2818_gpiolib_direction_output
Desc		: ���ö˿�Ϊ����������õ�ƽ
Params	: �������:
				chip		:rk2818������chip����&rk2818_gpio->chip
				offset	:�Ĵ���λƫ����	
				val		:0--�͵�ƽ;1--�ߵ�ƽ
		  �������:
				��
Return	:
		  0	--- �ɹ�
		  -1	--- ʧ��
----------------------------------------------------------------------*/
static int rk2818_gpiolib_direction_output(struct gpio_chip *chip,
					 unsigned offset, int val)
{
	unsigned	mask = offset_to_mask(offset);
	
	if(GPIOSetPinDirection(chip,mask,GPIO_OUT) == 0)
	{
		return GPIOSetPinLevel(chip,mask,val);
	}
	else
	{
		return -1;
	}
}

/*----------------------------------------------------------------------
Name	: rk2818_gpiolib_PullUpDown
Desc		: �����еĽ�λ���ж˿�������������ʼ��
Params	: �������:
				chip		:rk2818������chip����&rk2818_gpio->chip
				offset	:�Ĵ���λƫ��ֵ
				GPIOPullUpDown	:0--	����;1--����;2--����;3--δ��ʼ��		
		  �������:
				��
Return	:
		  0	--- �ɹ�
		  -1	--- ʧ��
----------------------------------------------------------------------*/
static int rk2818_gpiolib_PullUpDown(struct gpio_chip *chip, unsigned offset, unsigned val)
{
	return GPIOPullUpDown(chip, offset, val);
}
/*----------------------------------------------------------------------
Name	: rk2818_gpiolib_get
Desc		: ��ȡ�˿ڵ�ƽ
Params	: �������:
				chip		:rk2818������chip����&rk2818_gpio->chip
				offset	:�Ĵ���λƫ��ֵ	
		  �������:
				��
Return	:
		  0	--- �͵�ƽ
		  1	--- �ߵ�ƽ
		  -1	--- ʧ��
----------------------------------------------------------------------*/
static int rk2818_gpiolib_get(struct gpio_chip *chip, unsigned offset)
{
	unsigned	mask = offset_to_mask(offset);
	
	return GPIOGetPinLevel(chip,mask);
}

/*----------------------------------------------------------------------
Name	: rk2818_gpiolib_set
Desc		: ���ö˿ڵ�ƽ
Params	: �������:
				chip		:rk2818������chip����&rk2818_gpio->chip
				offset	:�Ĵ���λƫ��ֵ					
		  �������:
				��
Return	:
		 ��
----------------------------------------------------------------------*/
static void rk2818_gpiolib_set(struct gpio_chip *chip, unsigned offset, int val)
{
	unsigned	mask = offset_to_mask(offset);
	
	GPIOSetPinLevel(chip,mask,val);
	return;
}

/*----------------------------------------------------------------------
Name	: rk2818_gpiolib_to_irq
Desc		: ��GPIOתΪIRQ
Params	: �������:
				chip		:rk2818������chip����&rk2818_gpio->chip
				offset	:PINƫ��ֵ
				level		:��ƽ	 0:low 1:high					
		  �������:
				��
Return	:
		  ����ֵ	--- ��Ӧ��IRQֵ
		  -6	--- ʧ��
----------------------------------------------------------------------*/
static int rk2818_gpiolib_to_irq(struct gpio_chip *chip,
						unsigned offset)
{
    struct rk2818_gpio_chip *rk2818_gpio = to_rk2818_gpio_chip(chip);

    if(!rk2818_gpio)
    {
    	 return -1;
    }
    if(rk2818_gpio->bank->id==RK2818_ID_PIOA)
    {
        return offset + NR_AIC_IRQS;
    }
    else if(rk2818_gpio->bank->id==RK2818_ID_PIOE)
    {
        return offset + NR_AIC_IRQS + NUM_GROUP;
    }
    else
    {
        return -1;
    }
}

/*----------------------------------------------------------------------
Name	: rk2818_gpiolib_dbg_show
Desc		: DBG
Params	: �������:	
				s		:�ļ�
				chip		:rk2818������chip����&rk2818_gpio->chip				
		  �������:
				��
Return	:
		 ��
----------------------------------------------------------------------*/
static void rk2818_gpiolib_dbg_show(struct seq_file *s, struct gpio_chip *chip)
{

	int i;

	for (i = 0; i < chip->ngpio; i++) {
		unsigned pin = chip->base + i;
		struct gpio_chip *chip = pin_to_gpioChip(pin);
		unsigned mask = pin_to_mask(pin);
		const char *gpio_label;
		
		if(!chip ||!mask)
			return;
		
		gpio_label = gpiochip_is_requested(chip, i);
		if (gpio_label) {
			seq_printf(s, "[%s] GPIO%s%d: ",
				   gpio_label, chip->label, i);
			
			if (!chip || !mask)
			{
				seq_printf(s, "!chip || !mask\t");
				return;
			}
				
			GPIOSetPinDirection(chip,mask,GPIO_IN);
			seq_printf(s, "pin=%d,level=%d\t", pin,GPIOGetPinLevel(chip,mask));
			seq_printf(s, "\t");
		}
	}

	return;
}

static struct sysdev_class rk2818_gpio_sysclass = {
	.name		= "gpio",
	.suspend	= rk2818_gpio_suspend,
	.resume		= rk2818_gpio_resume,
};

static struct sys_device rk2818_gpio_device = {
	.cls		= &rk2818_gpio_sysclass,
};

static int __init rk2818_gpio_sysinit(void)
{
	int ret = sysdev_class_register(&rk2818_gpio_sysclass);
	if (ret == 0)
		ret = sysdev_register(&rk2818_gpio_device);
	return ret;
}

arch_initcall(rk2818_gpio_sysinit);

 /*----------------------------------------------------------------------
Name	: rk2818_gpio_init
Desc		: enable GPIO pin support.
Params	: �������:
				data		:rk2818_gpio_bank�ṹ��
				nr_banks	:GPIO����				
		  �������:
				��
Return	:
		  ��
----------------------------------------------------------------------*/
void __init rk2818_gpio_init(struct rk2818_gpio_bank *data, int nr_banks)
{
	unsigned	i;
	const char clkId[2][6] = {"gpio0","gpio1"};
	struct rk2818_gpio_chip *rk2818_gpio, *last = NULL;
	
	BUG_ON(nr_banks > MAX_GPIO_BANKS);

	gpio_banks = nr_banks;
	gpio_banksInGrp = nr_banks/2;
	
	for (i = 0; i < nr_banks; i++) {
		
		rk2818_gpio = &rk2818gpio_chip[i];
		rk2818_gpio->bank = &data[i];
		rk2818_gpio->bank->clock = clk_get(NULL,clkId[i/gpio_banksInGrp]);
		rk2818_gpio->regbase = (unsigned char  __iomem *)rk2818_gpio->bank->offset;

		/* enable gpio controller's clock */
		if(i%gpio_banksInGrp == 0)
			clk_enable(rk2818_gpio->bank->clock);
		
		if(last)
			last->next = rk2818_gpio;
		last = rk2818_gpio;

		gpiochip_add(&rk2818_gpio->chip);
	}
	pr_info("rk2818_gpio_init:nr_banks=%d\n",nr_banks);
	return;
}


#if 0
//#ifdef CONFIG_DEBUG_FS

static int rk2818_gpio_show(struct seq_file *s, void *unused)
{
	int bank, j;
	//printk(KERN_INFO "rk2818_gpio_show\n");
	/* print heading */
	seq_printf(s, "Pin\t");
	for (bank = 0; bank < gpio_banks; bank++) {
		seq_printf(s, "PIO%c\t", 'A' + bank);
	};
	seq_printf(s, "\n\n");

	/* print pin status */
	for (j = 0; j < NUM_GROUP; j++) {
		seq_printf(s, "%d:\t", j);

		for (bank = 0; bank < gpio_banks; bank++) {
			unsigned	pin  = PIN_BASE + (NUM_GROUP * bank) + j;
			struct gpio_chip *chip = pin_to_gpioChip(pin);
			unsigned	mask = pin_to_mask(pin);
			
			if (!chip ||!mask)
			{
				seq_printf(s, "!chip || !mask\t");
				//printk(KERN_INFO "rk2818_gpio_show:!chip || !mask\n");
				return -1;
			}
				
			GPIOSetPinDirection(chip,mask,GPIO_IN);
			seq_printf(s, "pin=%d,level=%d\t", pin,GPIOGetPinLevel(chip,mask));
			seq_printf(s, "\t");
		}
		seq_printf(s, "\n");
	}

	return 0;
}

static int rk2818_gpio_open(struct inode *inode, struct file *file)
{
	return single_open(file, rk2818_gpio_show, NULL);
}

static const struct file_operations rk2818_gpio_operations = {
	.open		= rk2818_gpio_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int __init rk2818_gpio_debugfs_init(void)
{
	/* /sys/kernel/debug/rk2818_gpio */
	//printk(KERN_INFO "rk2818_gpio_debugfs_init\n");

	(void) debugfs_create_file("rk2818_gpio", S_IFREG | S_IRUGO, NULL, NULL, &rk2818_gpio_operations);
	return 0;
}
postcore_initcall(rk2818_gpio_debugfs_init);

#endif
