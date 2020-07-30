#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/of_address.h>
#include <linux/kobject.h>

#include <linux/uaccess.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/cdev.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>
#include <linux/slab.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/fb.h>
#include <linux/notifier.h>

#include <linux/ioctl.h>
#include <linux/fs.h>
#include <linux/pm_runtime.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/property.h>
#if IS_ENABLED(CONFIG_COMPAT)
#include <linux/uaccess.h>
#include <linux/compat.h>
#endif

#include "mt_smi.h"

#include "smi_reg.h"
#include "smi_common.h"
#include "smi_debug.h"

#include "smi_priv.h"
#include "m4u.h"

/*#include "mmdvfs_mgr.h"*/

#define SMI_LOG_TAG "SMI"

#define LARB_BACKUP_REG_SIZE 128

#define SMI_COMMON_BACKUP_REG_NUM   10

/*
 * SMI COMMON register list to be backuped
 * for some socs which do not have some register, it's OK to read and write to
 * the non-exist offset.
 */
static unsigned short g_smi_common_backup_reg_offset[SMI_COMMON_BACKUP_REG_NUM] = {
	0x200, 0x204, 0x208, 0x20c, 0x210, 0x214, 0x220, 0x230, 0x234, 0x238
};

#define SF_HWC_PIXEL_MAX_NORMAL  (2560 * 1600 * 7)
#define SF_HWC_PIXEL_MAX_VR   (2560 * 1600 * 7)
#define SF_HWC_PIXEL_MAX_VP   (2560 * 1600 * 7)
#define SF_HWC_PIXEL_MAX_ALWAYS_GPU  (2560 * 1600 * 1)

#define SMIDBG(level, x...)            \
		do { if (smi_debug_level >= (level))\
			SMIMSG(x);\
		} while (0)

struct SMI_struct {
	spinlock_t SMI_lock;
	/*one bit represent one module */
	unsigned int pu4ConcurrencyTable[SMI_BWC_SCEN_CNT];
};

static struct SMI_struct g_SMIInfo;

static struct device *smiDeviceUevent;

static bool fglarbcallback;	/*larb backuprestore */

struct mtk_smi_data *smi_data;

static struct cdev *pSmiDev;

static unsigned int g_smi_common_backup[SMI_COMMON_BACKUP_REG_NUM];

/* To keep the HW's init value*/
static bool is_default_value_saved;
static unsigned int default_val_smi_l1arb[SMI_LARB_NR_MAX] = { 0 };

static unsigned int wifi_disp_transaction;

/* debug level */
static unsigned int smi_debug_level;

/* tuning mode, 1 for register ioctl */
static unsigned int smi_tuning_mode;

static unsigned int smi_profile = SMI_BWC_SCEN_NORMAL;

static unsigned int *pLarbRegBackUp[SMI_LARB_NR_MAX];
static int g_bInited;

static MTK_SMI_BWC_MM_INFO g_smi_bwc_mm_info = { 0, 0, {0, 0}, {0, 0},
{0, 0}, {0, 0}, 0, 0, 0,
SF_HWC_PIXEL_MAX_NORMAL
};

struct mtk_smi_common {
	void __iomem		*base;
	struct clk		*clk_apb;
	struct clk		*clk_smi;
};
struct mtk_smi_larb {
	void __iomem		*base;
	struct clk		*clk_apb;
	struct clk		*clk_smi;
	struct device		*smi;
};

static void smi_dumpLarb(unsigned int index);
static void smi_dumpCommon(void);
static int _mtk_smi_larb_get(struct device *larbdev, bool pm);
static void _mtk_smi_larb_put(struct device *larbdev, bool pm);

#if IS_ENABLED(CONFIG_COMPAT)
static long MTK_SMI_COMPAT_ioctl(struct file *filp, unsigned int cmd, unsigned long arg);
#else
#define MTK_SMI_COMPAT_ioctl  NULL
#endif

/* Use this function to get base address of Larb resgister
* to support error checking
*/
static unsigned long get_larb_base_addr(int larb_id)
{
	if (!smi_data || larb_id > smi_data->larb_nr || larb_id < 0)
		return SMI_ERROR_ADDR;
	else
		return smi_data->larb_base[larb_id];
}

unsigned long mtk_smi_larb_get_base(int larbid)
{
	return get_larb_base_addr(larbid);
}

static unsigned int smi_get_larb_index(struct device *dev)
{
	unsigned int idx;

	for (idx = 0; idx < smi_data->larb_nr; idx++) {
		if (smi_data->larb[idx] == dev)
			break;
	}
	return idx;
}

int mtk_smi_larb_clock_on(int larbid, bool pm)
{
	if (!smi_data || larbid < 0 || larbid >= smi_data->larb_nr)
		return -EINVAL;

	return _mtk_smi_larb_get(smi_data->larb[larbid], pm);
}

void mtk_smi_larb_clock_off(int larbid, bool pm)
{
	if (!smi_data || larbid < 0 || larbid >= smi_data->larb_nr)
		return;

	_mtk_smi_larb_put(smi_data->larb[larbid], pm);
}

static void backup_smi_common(void)
{
	int i;

	for (i = 0; i < SMI_COMMON_BACKUP_REG_NUM; i++) {
		g_smi_common_backup[i] =
		    M4U_ReadReg32(SMI_COMMON_EXT_BASE,
				  (unsigned long)g_smi_common_backup_reg_offset[i]);
	}
}

static void restore_smi_common(void)
{
	int i;

	for (i = 0; i < SMI_COMMON_BACKUP_REG_NUM; i++) {
		M4U_WriteReg32(SMI_COMMON_EXT_BASE,
			       (unsigned long)g_smi_common_backup_reg_offset[i],
			       g_smi_common_backup[i]);
	}
}

static void backup_larb_smi(int index)
{
	int port_index = 0;
	unsigned short int *backup_ptr = NULL;
	unsigned long larb_base = get_larb_base_addr(index);
	unsigned long larb_offset = 0x200;
	int total_port_num = 0;

	/* boundary check for larb_port_num and larb_port_backup access */
	if (index < 0 || index > smi_data->larb_nr)
		return;

	total_port_num = smi_data->smi_priv->larb_port_num[index];
	backup_ptr = smi_data->larb_port_backup + index*SMI_LARB_PORT_NR_MAX;

	/* boundary check for port value access */
	if (total_port_num <= 0 || backup_ptr == NULL)
		return;

	for (port_index = 0; port_index < total_port_num; port_index++) {
		*backup_ptr = (unsigned short int)(M4U_ReadReg32(larb_base, larb_offset));
		backup_ptr++;
		larb_offset += 4;
	}

	/* backup smi common along with larb0,
	 * smi common clk is guaranteed to be on when processing larbs */
	if (index == 0)
		backup_smi_common();

}


static void restore_larb_smi(int index)
{
	int port_index = 0;
	unsigned short int *backup_ptr = NULL;
	unsigned long larb_base = get_larb_base_addr(index);
	unsigned long larb_offset = 0x200;
	unsigned int backup_value = 0;
	int total_port_num = 0;

	/* boundary check for larb_port_num and larb_port_backup access */
	if (index < 0 || index > smi_data->larb_nr)
		return;

	total_port_num = smi_data->smi_priv->larb_port_num[index];
	backup_ptr = smi_data->larb_port_backup + index*SMI_LARB_PORT_NR_MAX;

	/* boundary check for port value access */
	if (total_port_num <= 0 || backup_ptr == NULL)
		return;

	/* restore smi common along with larb0,
	 * smi common clk is guaranteed to be on when processing larbs */
	if (index == 0)
		restore_smi_common();

	for (port_index = 0; port_index < total_port_num; port_index++) {
		backup_value = *backup_ptr;
		M4U_WriteReg32(larb_base, larb_offset, backup_value);
		backup_ptr++;
		larb_offset += 4;
	}

	/* we do not backup 0x20 because it is a fixed setting */
	if (smi_data->smi_priv->plat == MTK_PLAT_MT8173 || smi_data->smi_priv->plat == MTK_PLAT_MT8163)
		M4U_WriteReg32(larb_base, 0x20, smi_data->smi_priv->larb_vc_setting[index]);

	/* turn off EMI empty OSTD dobule, fixed setting */
	M4U_WriteReg32(larb_base, 0x2c, 4);

}

static int larb_reg_backup(int larb)
{
	unsigned int *pReg = pLarbRegBackUp[larb];
	unsigned long larb_base = get_larb_base_addr(larb);

	*(pReg++) = M4U_ReadReg32(larb_base, SMI_LARB_CON);

	/* *(pReg++) = M4U_ReadReg32(larb_base, SMI_SHARE_EN); */
	/* *(pReg++) = M4U_ReadReg32(larb_base, SMI_ROUTE_SEL); */

	backup_larb_smi(larb);

	if (0 == larb)
		g_bInited = 0;
	if (smi_data->smi_priv->plat == MTK_PLAT_MT8173 || smi_data->smi_priv->plat == MTK_PLAT_MT8163)
		m4u_larb_backup_sec(larb);

	return 0;
}

static int smi_larb_init(unsigned int larb)
{
	unsigned int regval = 0;
	unsigned int regval1 = 0;
	unsigned int regval2 = 0;
	unsigned long larb_base = get_larb_base_addr(larb);

	/* Clock manager enable LARB clock before call back restore already,
	 *it will be disabled after restore call back returns
	 * Got to enable OSTD before engine starts */
	regval = M4U_ReadReg32(larb_base, SMI_LARB_STAT);

	/*todo */
	/* regval1 = M4U_ReadReg32(larb_base , SMI_LARB_MON_BUS_REQ0); */
	/* regval2 = M4U_ReadReg32(larb_base , SMI_LARB_MON_BUS_REQ1); */

	if (0 == regval) {
		SMIDBG(1, "Init OSTD for larb_base: 0x%lx\n", larb_base);
		M4U_WriteReg32(larb_base, SMI_LARB_OSTDL_SOFT_EN, 0xffffffff);
	} else {
		SMIMSG("Larb: 0x%lx is busy : 0x%x , port:0x%x,0x%x ,fail to set OSTD\n", larb_base,
		       regval, regval1, regval2);
		smi_dumpDebugMsg();
		if (smi_debug_level >= 1) {
			SMIERR("DISP_MDP LARB  0x%lx OSTD cannot be set:0x%x,port:0x%x,0x%x\n",
			       larb_base, regval, regval1, regval2);
		} else {
			dump_stack();
		}
	}

	restore_larb_smi(larb);

	return 0;
}

int larb_reg_restore(int larb)
{
	unsigned long larb_base = SMI_ERROR_ADDR;
	unsigned int regval = 0;
	unsigned int *pReg = NULL;

	larb_base = get_larb_base_addr(larb);

	/* The larb assign doesn't exist */
	if (larb_base == SMI_ERROR_ADDR) {
		SMIMSG("Can't find the base address for Larb%d\n", larb);
		return 0;
	}

	pReg = pLarbRegBackUp[larb];

	SMIDBG(1, "+larb_reg_restore(), larb_idx=%d\n", larb);
	SMIDBG(1, "m4u part restore, larb_idx=%d\n", larb);
	/*warning: larb_con is controlled by set/clr */
	regval = *(pReg++);
	M4U_WriteReg32(larb_base, SMI_LARB_CON_CLR, ~(regval));
	M4U_WriteReg32(larb_base, SMI_LARB_CON_SET, (regval));

	/*M4U_WriteReg32(larb_base, SMI_SHARE_EN, *(pReg++) ); */
	/*M4U_WriteReg32(larb_base, SMI_ROUTE_SEL, *(pReg++) ); */

	smi_larb_init(larb);
	if (smi_data->smi_priv->plat == MTK_PLAT_MT8173 || smi_data->smi_priv->plat == MTK_PLAT_MT8163)
		m4u_larb_restore_sec(larb);

	return 0;
}

/* Fake mode check, e.g. WFD */
static int fake_mode_handling(MTK_SMI_BWC_CONFIG *p_conf, unsigned int *pu4LocalCnt)
{
	if (p_conf->scenario == SMI_BWC_SCEN_WFD) {
		if (p_conf->b_on_off) {
			wifi_disp_transaction = 1;
			SMIMSG("Enable WFD in profile: %d\n", smi_profile);
		} else {
			wifi_disp_transaction = 0;
			SMIMSG("Disable WFD in profile: %d\n", smi_profile);
		}
		return 1;
	} else {
		return 0;
	}
}

static int ovl_limit_uevent(int bwc_scenario, int ovl_pixel_limit)
{
	int err = 0;
	char *envp[3];
	char scenario_buf[32] = "";
	char ovl_limit_buf[32] = "";

	/* scenario_buf = kzalloc(sizeof(char)*128, GFP_KERNEL); */
	/* ovl_limit_buf = kzalloc(sizeof(char)*128, GFP_KERNEL); */

	snprintf(scenario_buf, 31, "SCEN=%d", bwc_scenario);
	snprintf(ovl_limit_buf, 31, "HWOVL=%d", ovl_pixel_limit);

	envp[0] = scenario_buf;
	envp[1] = ovl_limit_buf;
	envp[2] = NULL;

	if (pSmiDev != NULL) {
		/* err = kobject_uevent_env(&(pSmiDev->kobj), KOBJ_CHANGE, envp); */
		/* use smi_data->dev.lobj instead */
		/* err = kobject_uevent_env(&(smi_data->dev->kobj), KOBJ_CHANGE, envp); */
		/* user smiDeviceUevent->kobj instead */
		err = kobject_uevent_env(&(smiDeviceUevent->kobj), KOBJ_CHANGE, envp);
		SMIMSG("Notify OVL limitaion=%d, SCEN=%d", ovl_pixel_limit, bwc_scenario);
	}
	/* kfree(scenario_buf); */
	/* kfree(ovl_limit_buf); */

	if (err < 0)
		SMIMSG(KERN_INFO "[%s] kobject_uevent_env error = %d\n", __func__, err);

	return err;
}

static int smi_bwc_config(MTK_SMI_BWC_CONFIG *p_conf, unsigned int *pu4LocalCnt)
{
	int i;
	int result = 0;
	unsigned int u4Concurrency = 0;
	MTK_SMI_BWC_SCEN eFinalScen;
	static MTK_SMI_BWC_SCEN ePreviousFinalScen = SMI_BWC_SCEN_CNT;
	struct mtk_smi_priv *smicur = (struct mtk_smi_priv *)smi_data->smi_priv;

	if (smi_tuning_mode == 1) {
		SMIMSG("Doesn't change profile in tunning mode");
		return 0;
	}

	spin_lock(&g_SMIInfo.SMI_lock);
	result = fake_mode_handling(p_conf, pu4LocalCnt);
	spin_unlock(&g_SMIInfo.SMI_lock);

	/* Fake mode is not a real SMI profile, so we need to return here */
	if (result == 1)
		return 0;

	if ((SMI_BWC_SCEN_CNT <= p_conf->scenario) || (0 > p_conf->scenario)) {
		SMIERR("Incorrect SMI BWC config : 0x%x, how could this be...\n", p_conf->scenario);
		return -1;
	}
/* Debug - S */
/* SMIMSG("SMI setTo%d,%s,%d\n" , p_conf->scenario , (p_conf->b_on_off ? "on" : "off") , ePreviousFinalScen); */
/* Debug - E */
#if 0
	if (p_conf->b_on_off) {
		/* set mmdvfs step according to certain scenarios */
		mmdvfs_notify_scenario_enter(p_conf->scenario);
	} else {
		/* set mmdvfs step to default after the scenario exits */
		mmdvfs_notify_scenario_exit(p_conf->scenario);
	}
#endif
	/* turn on larb clock */
	for (i = 0; i <= smi_data->larb_nr; i++)
		mtk_smi_larb_clock_on(i, true);

	spin_lock(&g_SMIInfo.SMI_lock);

	if (p_conf->b_on_off) {
		/* turn on certain scenario */
		g_SMIInfo.pu4ConcurrencyTable[p_conf->scenario] += 1;

		if (NULL != pu4LocalCnt)
			pu4LocalCnt[p_conf->scenario] += 1;
	} else {
		/* turn off certain scenario */
		if (0 == g_SMIInfo.pu4ConcurrencyTable[p_conf->scenario]) {
			SMIMSG("Too many turning off for global SMI profile:%d,%d\n",
			       p_conf->scenario, g_SMIInfo.pu4ConcurrencyTable[p_conf->scenario]);
		} else {
			g_SMIInfo.pu4ConcurrencyTable[p_conf->scenario] -= 1;
		}

		if (NULL != pu4LocalCnt) {
			if (0 == pu4LocalCnt[p_conf->scenario]) {
				SMIMSG
				    ("Process : %s did too many turning off for local SMI profile:%d,%d\n",
				     current->comm, p_conf->scenario,
				     pu4LocalCnt[p_conf->scenario]);
			} else {
				pu4LocalCnt[p_conf->scenario] -= 1;
			}
		}
	}

	for (i = 0; i < SMI_BWC_SCEN_CNT; i++) {
		if (g_SMIInfo.pu4ConcurrencyTable[i])
			u4Concurrency |= (1 << i);
	}

	if ((1 << SMI_BWC_SCEN_MM_GPU) & u4Concurrency)
		eFinalScen = SMI_BWC_SCEN_MM_GPU;
	else if ((1 << SMI_BWC_SCEN_VR_SLOW) & u4Concurrency)
		eFinalScen = SMI_BWC_SCEN_VR_SLOW;
	else if ((1 << SMI_BWC_SCEN_VR) & u4Concurrency)
		eFinalScen = SMI_BWC_SCEN_VR;
	else if ((1 << SMI_BWC_SCEN_ICFP) & u4Concurrency)
		eFinalScen = SMI_BWC_SCEN_VR;
	else if ((1 << SMI_BWC_SCEN_VP) & u4Concurrency)
		eFinalScen = SMI_BWC_SCEN_VP;
	else if ((1 << SMI_BWC_SCEN_SWDEC_VP) & u4Concurrency)
		eFinalScen = SMI_BWC_SCEN_SWDEC_VP;
	else if ((1 << SMI_BWC_SCEN_VENC) & u4Concurrency)
		eFinalScen = SMI_BWC_SCEN_VENC;
	else if ((1 << SMI_BWC_SCEN_HDMI) & u4Concurrency)
		eFinalScen = SMI_BWC_SCEN_HDMI;
	else if ((1 << SMI_BWC_SCEN_HDMI4K) & u4Concurrency)
		eFinalScen = SMI_BWC_SCEN_HDMI4K;
	else
		eFinalScen = SMI_BWC_SCEN_NORMAL;

	if (ePreviousFinalScen == eFinalScen) {
		SMIMSG("Scen equal%d,don't change\n", eFinalScen);
		goto err_clkoff;
	} else {
		ePreviousFinalScen = eFinalScen;
	}

	smi_profile = eFinalScen;

	/* Bandwidth Limiter */
	switch (eFinalScen) {
	case SMI_BWC_SCEN_VP:
		SMIMSG("[SMI_PROFILE] : %s\n", "SMI_BWC_VP");
		if (smi_data->smi_priv->plat != MTK_PLAT_MT8163)
			smicur->vp_setting(smi_data);
		else {
			if (wifi_disp_transaction)
				smicur->vp_setting(smi_data);
			else
				smicur->vp_wfd_setting(smi_data);
		}

		g_smi_bwc_mm_info.hw_ovl_limit = SF_HWC_PIXEL_MAX_VP;
		break;

	case SMI_BWC_SCEN_SWDEC_VP:
		SMIMSG("[SMI_PROFILE] : %s\n", "SMI_BWC_SCEN_SWDEC_VP");
		smicur->vp_setting(smi_data);
		g_smi_bwc_mm_info.hw_ovl_limit = SF_HWC_PIXEL_MAX_VP;
		break;

	case SMI_BWC_SCEN_VR:
		SMIMSG("[SMI_PROFILE] : %s\n", "SMI_BWC_VR");
		smicur->vr_setting(smi_data);
		g_smi_bwc_mm_info.hw_ovl_limit = SF_HWC_PIXEL_MAX_VR;
		break;

	case SMI_BWC_SCEN_VR_SLOW:
		SMIMSG("[SMI_PROFILE] : %s\n", "SMI_BWC_VR");
		smi_profile = SMI_BWC_SCEN_VR_SLOW;
		smicur->vr_setting(smi_data);
		g_smi_bwc_mm_info.hw_ovl_limit = SF_HWC_PIXEL_MAX_VR;
		break;

	case SMI_BWC_SCEN_VENC:
		SMIMSG("[SMI_PROFILE] : %s\n", "SMI_BWC_SCEN_VENC");
		smicur->vr_setting(smi_data);
		g_smi_bwc_mm_info.hw_ovl_limit = SF_HWC_PIXEL_MAX_NORMAL;
		break;

	case SMI_BWC_SCEN_NORMAL:
		SMIMSG("[SMI_PROFILE] : %s\n", "SMI_BWC_SCEN_NORMAL");
		g_smi_bwc_mm_info.hw_ovl_limit = SF_HWC_PIXEL_MAX_NORMAL;
		smicur->init_setting(smi_data, &is_default_value_saved,
				default_val_smi_l1arb, smi_data->larb_nr);
		break;

	case SMI_BWC_SCEN_MM_GPU:
		SMIMSG("[SMI_PROFILE] : %s\n", "SMI_BWC_SCEN_MM_GPU");
		g_smi_bwc_mm_info.hw_ovl_limit = SF_HWC_PIXEL_MAX_NORMAL;
		smicur->init_setting(smi_data, &is_default_value_saved,
				default_val_smi_l1arb, smi_data->larb_nr);
		break;

	case SMI_BWC_SCEN_HDMI:
		SMIMSG("[SMI_PROFILE] : %s\n", "SMI_BWC_SCEN_HDMI");
		g_smi_bwc_mm_info.hw_ovl_limit = SF_HWC_PIXEL_MAX_NORMAL;
		smicur->hdmi_setting(smi_data);
		break;

	case SMI_BWC_SCEN_HDMI4K:
		SMIMSG("[SMI_PROFILE] : %s\n", "SMI_BWC_SCEN_HDMI4K");
		g_smi_bwc_mm_info.hw_ovl_limit = SF_HWC_PIXEL_MAX_NORMAL;
		smicur->hdmi_4k_setting(smi_data);
		break;

	default:
		SMIMSG("[SMI_PROFILE] : %s %d\n", "initSetting", eFinalScen);
		smicur->init_setting(smi_data, &is_default_value_saved,
				default_val_smi_l1arb, smi_data->larb_nr);
		g_smi_bwc_mm_info.hw_ovl_limit = SF_HWC_PIXEL_MAX_NORMAL;
		break;
	}

	spin_unlock(&g_SMIInfo.SMI_lock);

	/*turn off larb clock */
	for (i = 0; i <= smi_data->larb_nr; i++)
		mtk_smi_larb_clock_off(i, true);

	/* Since send uevent may trigger sleeping, we must send the event after releasing spin lock */
	ovl_limit_uevent(smi_profile, g_smi_bwc_mm_info.hw_ovl_limit);

	SMIMSG("SMI_PROFILE to:%d %s,cur:%d,%d,%d,%d\n", p_conf->scenario,
	       (p_conf->b_on_off ? "on" : "off"), eFinalScen,
	       g_SMIInfo.pu4ConcurrencyTable[SMI_BWC_SCEN_NORMAL],
	       g_SMIInfo.pu4ConcurrencyTable[SMI_BWC_SCEN_VR],
	       g_SMIInfo.pu4ConcurrencyTable[SMI_BWC_SCEN_VP]);

	return 0;

/* Debug usage - S */
/* smi_dumpDebugMsg(); */
/* SMIMSG("Config:%d,%d,%d\n" , eFinalScen ,
*g_SMIInfo.pu4ConcurrencyTable[SMI_BWC_SCEN_NORMAL] ,
*(NULL == pu4LocalCnt ? (-1) : pu4LocalCnt[p_conf->scenario])); */
/* Debug usage - E */

err_clkoff:
	spin_unlock(&g_SMIInfo.SMI_lock);

	/*turn off larb clock */
	for (i = 0; i <= smi_data->larb_nr; i++)
		mtk_smi_larb_clock_off(i, true);
	return 0;
}

/*
const struct dev_pm_ops mtk_smi_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(smiclk_subsys_before_off, smiclk_subsys_after_on)
};*/

int smi_common_init(void)
{
	int i;

	for (i = 0; i <= smi_data->larb_nr; i++) {
		pLarbRegBackUp[i] = kmalloc(LARB_BACKUP_REG_SIZE, GFP_KERNEL | __GFP_ZERO);
		if (pLarbRegBackUp[i] == NULL)
			SMIERR("pLarbRegBackUp kmalloc fail %d\n", i);
	}

	for (i = 0; i < smi_data->larb_nr; i++)
		mtk_smi_larb_clock_on(i, true);

	/* apply init setting after kernel boot */
	smi_data->smi_priv->init_setting(smi_data, &is_default_value_saved,
			default_val_smi_l1arb, smi_data->larb_nr);


	fglarbcallback = true;

	for (i = smi_data->larb_nr; i >= 0; i--)
		mtk_smi_larb_clock_off(i, true);

	return 0;
}

static int smi_open(struct inode *inode, struct file *file)
{
	file->private_data = kmalloc_array(SMI_BWC_SCEN_CNT, sizeof(unsigned int), GFP_ATOMIC);

	if (NULL == file->private_data) {
		SMIMSG("Not enough entry for DDP open operation\n");
		return -ENOMEM;
	}

	memset(file->private_data, 0, SMI_BWC_SCEN_CNT * sizeof(unsigned int));

	return 0;
}

static int smi_release(struct inode *inode, struct file *file)
{
	if (NULL != file->private_data) {
		kfree(file->private_data);
		file->private_data = NULL;
	}

	return 0;
}

/* GMP start */

void smi_bwc_mm_info_set(int property_id, long val1, long val2)
{

	switch (property_id) {
	case SMI_BWC_INFO_CON_PROFILE:
		g_smi_bwc_mm_info.concurrent_profile = (int)val1;
		break;
	case SMI_BWC_INFO_SENSOR_SIZE:
		g_smi_bwc_mm_info.sensor_size[0] = val1;
		g_smi_bwc_mm_info.sensor_size[1] = val2;
		break;
	case SMI_BWC_INFO_VIDEO_RECORD_SIZE:
		g_smi_bwc_mm_info.video_record_size[0] = val1;
		g_smi_bwc_mm_info.video_record_size[1] = val2;
		break;
	case SMI_BWC_INFO_DISP_SIZE:
		g_smi_bwc_mm_info.display_size[0] = val1;
		g_smi_bwc_mm_info.display_size[1] = val2;
		break;
	case SMI_BWC_INFO_TV_OUT_SIZE:
		g_smi_bwc_mm_info.tv_out_size[0] = val1;
		g_smi_bwc_mm_info.tv_out_size[1] = val2;
		break;
	case SMI_BWC_INFO_FPS:
		g_smi_bwc_mm_info.fps = (int)val1;
		break;
	case SMI_BWC_INFO_VIDEO_ENCODE_CODEC:
		g_smi_bwc_mm_info.video_encode_codec = (int)val1;
		break;
	case SMI_BWC_INFO_VIDEO_DECODE_CODEC:
		g_smi_bwc_mm_info.video_decode_codec = (int)val1;
		break;
	}
}

/* GMP end */

static long smi_ioctl(struct file *pFile, unsigned int cmd, unsigned long param)
{
	int ret = 0;
/* unsigned long * pu4Cnt = (unsigned long *)pFile->private_data; */

	switch (cmd) {

	case MTK_IOC_SMI_BWC_CONFIG:
		{
			MTK_SMI_BWC_CONFIG cfg;

			ret = copy_from_user(&cfg, (void *)param, sizeof(MTK_SMI_BWC_CONFIG));
			if (ret) {
				SMIMSG(" SMI_BWC_CONFIG, copy_from_user failed: %d\n", ret);
				return -EFAULT;
			}

			ret = smi_bwc_config(&cfg, NULL);
		}
		break;
		/* GMP start */
	case MTK_IOC_SMI_BWC_INFO_SET:
		{
			MTK_SMI_BWC_INFO_SET cfg;
			/* SMIMSG("Handle MTK_IOC_SMI_BWC_INFO_SET request... start"); */
			ret = copy_from_user(&cfg, (void *)param, sizeof(MTK_SMI_BWC_INFO_SET));
			if (ret) {
				SMIMSG(" MTK_IOC_SMI_BWC_INFO_SET, copy_to_user failed: %d\n", ret);
				return -EFAULT;
			}
			/* Set the address to the value assigned by user space program */
			smi_bwc_mm_info_set(cfg.property, cfg.value1, cfg.value2);
			/* SMIMSG("Handle MTK_IOC_SMI_BWC_INFO_SET request... finish"); */
			break;
		}
	case MTK_IOC_SMI_BWC_INFO_GET:
		{
			ret = copy_to_user((void *)param, (void *)&g_smi_bwc_mm_info,
					   sizeof(MTK_SMI_BWC_MM_INFO));

			if (ret) {
				SMIMSG(" MTK_IOC_SMI_BWC_INFO_GET, copy_to_user failed: %d\n", ret);
				return -EFAULT;
			}
			/* SMIMSG("Handle MTK_IOC_SMI_BWC_INFO_GET request... finish"); */
			break;
		}
		/* GMP end */

	case MTK_IOC_SMI_DUMP_LARB:
		{
			unsigned int larb_index;

			ret = copy_from_user(&larb_index, (void *)param, sizeof(unsigned int));
			if (ret)
				return -EFAULT;

			smi_dumpLarb(larb_index);
		}
		break;

	case MTK_IOC_SMI_DUMP_COMMON:
		{
			unsigned int arg;

			ret = copy_from_user(&arg, (void *)param, sizeof(unsigned int));
			if (ret)
				return -EFAULT;

			smi_dumpCommon();
		}
		break;

	/*case MTK_IOC_MMDVFS_CMD:
		{
			MTK_MMDVFS_CMD mmdvfs_cmd;

			if (copy_from_user(&mmdvfs_cmd, (void *)param, sizeof(MTK_MMDVFS_CMD)))
				return -EFAULT;

			mmdvfs_handle_cmd(&mmdvfs_cmd);

			if (copy_to_user
			    ((void *)param, (void *)&mmdvfs_cmd, sizeof(MTK_MMDVFS_CMD)))
				return -EFAULT;

			break;
	}*/
	default:
		return -1;
	}

	return ret;
}

static const struct file_operations smiFops = {
	.owner = THIS_MODULE,
	.open = smi_open,
	.release = smi_release,
	.unlocked_ioctl = smi_ioctl,
	.compat_ioctl = MTK_SMI_COMPAT_ioctl
};

static dev_t smiDevNo = MKDEV(MTK_SMI_MAJOR_NUMBER, 0);
static inline int smi_register(void)
{
	if (alloc_chrdev_region(&smiDevNo, 0, 1, "MTK_SMI")) {
		SMIERR("Allocate device No. failed");
		return -EAGAIN;
	}
	/* Allocate driver */
	pSmiDev = cdev_alloc();

	if (NULL == pSmiDev) {
		unregister_chrdev_region(smiDevNo, 1);
		SMIERR("Allocate mem for kobject failed");
		return -ENOMEM;
	}
	/* Attatch file operation. */
	cdev_init(pSmiDev, &smiFops);
	pSmiDev->owner = THIS_MODULE;

	/* Add to system */
	if (cdev_add(pSmiDev, smiDevNo, 1)) {
		SMIERR("Attatch file operation failed");
		unregister_chrdev_region(smiDevNo, 1);
		return -EAGAIN;
	}

	return 0;
}

static struct class *pSmiClass;
static int smi_dev_register(void)
{
	int ret;
	struct device *smiDevice = NULL;

	if (smi_register()) {
		pr_err("register SMI failed\n");
		return -EAGAIN;
	}

	pSmiClass = class_create(THIS_MODULE, "MTK_SMI");
	if (IS_ERR(pSmiClass)) {
		ret = PTR_ERR(pSmiClass);
		SMIERR("Unable to create class, err = %d", ret);
		return ret;
	}

	smiDevice = device_create(pSmiClass, NULL, smiDevNo, NULL, "MTK_SMI");
	smiDeviceUevent = smiDevice;

	return 0;
}

static int mtk_smi_common_get(struct device *smidev, bool pm)
{
	struct mtk_smi_common *smipriv = dev_get_drvdata(smidev);
	int ret;

	if (pm) {
		ret = pm_runtime_get_sync(smidev);
		if (ret < 0)
			return ret;
	}

	ret = clk_prepare_enable(smipriv->clk_apb);
	if (ret) {
		dev_err(smidev, "Failed to enable the apb clock\n");
		goto err_put_pm;
	}
	ret = clk_prepare_enable(smipriv->clk_smi);
	if (ret) {
		dev_err(smidev, "Failed to enable the smi clock\n");
		goto err_disable_apb;
	}
	return ret;

err_disable_apb:
	clk_disable_unprepare(smipriv->clk_apb);
err_put_pm:
	if (pm)
		pm_runtime_put_sync(smidev);
	return ret;
}

static void mtk_smi_common_put(struct device *smidev, bool pm)
{
	struct mtk_smi_common *smipriv = dev_get_drvdata(smidev);

	clk_disable_unprepare(smipriv->clk_smi);
	clk_disable_unprepare(smipriv->clk_apb);
	if (pm)
		pm_runtime_put_sync(smidev);
}

static int _mtk_smi_larb_get(struct device *larbdev, bool pm)
{
	struct mtk_smi_larb *larbpriv = dev_get_drvdata(larbdev);
	int ret;

	ret = mtk_smi_common_get(larbpriv->smi, pm);
	if (ret)
		return ret;

	if (pm) {
		ret = pm_runtime_get_sync(larbdev);
		if (ret < 0)
			goto err_put_smicommon;
	}

	ret = clk_prepare_enable(larbpriv->clk_apb);
	if (ret) {
		dev_err(larbdev, "Failed to enable the apb clock\n");
		goto err_put_pm;
	}

	ret = clk_prepare_enable(larbpriv->clk_smi);
	if (ret) {
		dev_err(larbdev, "Failed to enable the smi clock\n");
		goto err_disable_apb;
	}

	return ret;

err_disable_apb:
	clk_disable_unprepare(larbpriv->clk_apb);
err_put_pm:
	if (pm)
		pm_runtime_put_sync(larbdev);
err_put_smicommon:
	mtk_smi_common_put(larbpriv->smi, pm);
	return ret;
}

static void _mtk_smi_larb_put(struct device *larbdev, bool pm)
{
	struct mtk_smi_larb *larbpriv = dev_get_drvdata(larbdev);

	clk_disable_unprepare(larbpriv->clk_smi);
	clk_disable_unprepare(larbpriv->clk_apb);
	if (pm)
		pm_runtime_put_sync(larbdev);
	mtk_smi_common_put(larbpriv->smi, pm);
}

/* The power is alway on during power-domain callback.*/
static int mtk_smi_larb_runtime_suspend(struct device *dev)
{
	unsigned int idx = smi_get_larb_index(dev);
	int ret;

	if (!fglarbcallback)
		return 0;
	if (idx > smi_data->larb_nr)
		return 0;

	ret = _mtk_smi_larb_get(dev, false);
	if (ret) {
		dev_warn(dev, "runtime suspend clk-warn larb%d\n", idx);
		return 0;
	}

	larb_reg_backup(idx);

	_mtk_smi_larb_put(dev, false);
	return 0;
}

static int mtk_smi_larb_runtime_resume(struct device *dev)
{
	unsigned int idx = smi_get_larb_index(dev);
	int ret;

	if (!fglarbcallback)
		return 0;
	if (idx > smi_data->larb_nr)
		return 0;

	ret = _mtk_smi_larb_get(dev, false);
	if (ret) {
		dev_warn(dev, "runtime resume clk-warn larb%d\n", idx);
		return 0;
	}

	larb_reg_restore(idx);

	_mtk_smi_larb_put(dev, false);
	return 0;
}

/* modify this to avoid build error when runtime_pm not configured */
static const struct dev_pm_ops mtk_smi_larb_ops = {
	.runtime_suspend = mtk_smi_larb_runtime_suspend,
	.runtime_resume = mtk_smi_larb_runtime_resume,
};

static int mtk_smi_larb_probe(struct platform_device *pdev)
{
	struct mtk_smi_larb *larbpriv;
	struct resource *res;
	struct device *dev = &pdev->dev;
	struct device_node *smi_node;
	struct platform_device *smi_pdev;
	int ret, larbid;

	if (!dev->pm_domain)
		return -EPROBE_DEFER;

	larbpriv = devm_kzalloc(dev, sizeof(*larbpriv), GFP_KERNEL);
	if (!larbpriv)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	larbpriv->base = devm_ioremap_resource(dev, res);
	if (IS_ERR(larbpriv->base))
		return PTR_ERR(larbpriv->base);

	larbpriv->clk_apb = devm_clk_get(dev, "apb");
	if (IS_ERR(larbpriv->clk_apb))
		return PTR_ERR(larbpriv->clk_apb);

	larbpriv->clk_smi = devm_clk_get(dev, "smi");
	if (IS_ERR(larbpriv->clk_smi))
		return PTR_ERR(larbpriv->clk_smi);

	smi_node = of_parse_phandle(dev->of_node, "mediatek,smi", 0);
	if (!smi_node)
		return -EINVAL;

	ret = of_property_read_u32(dev->of_node, "mediatek,larbid", &larbid);
	if (ret)
		return ret;

	smi_pdev = of_find_device_by_node(smi_node);
	of_node_put(smi_node);
	if (smi_pdev) {
		larbpriv->smi = &smi_pdev->dev;
	} else {
		dev_err(dev, "Failed to get the smi_common device\n");
		return -EINVAL;
	}

	smi_data->larb_base[larbid] = (unsigned long)larbpriv->base;
	smi_data->larb[larbid] = dev;
	smi_data->larb_nr++;

	SMIMSG("larb %d-cnt %d probe done\n", larbid, smi_data->larb_nr);

	pm_runtime_enable(dev);
	dev_set_drvdata(dev, larbpriv);
	return 0;
}

static int mtk_smi_larb_remove(struct platform_device *pdev)
{
	pm_runtime_disable(&pdev->dev);
	return 0;
}

static const struct of_device_id mtk_smi_larb_of_ids[] = {
	{ .compatible = "mediatek,mt8173-smi-larb", },
	{ .compatible = "mediatek,mt8163-smi-larb", },
	{ .compatible = "mediatek,mt8127-smi-larb", },
	{}
};

static struct platform_driver mtk_smi_larb_driver = {
	.probe	= mtk_smi_larb_probe,
	.remove = mtk_smi_larb_remove,
	.driver	= {
		.name = "mtk-smi-larb",
		.of_match_table = mtk_smi_larb_of_ids,
#ifdef CONFIG_PM
		.pm = &mtk_smi_larb_ops,
#endif
	}
};

static int mtk_smi_probe(struct platform_device *pdev);
static int mtk_smi_remove(struct platform_device *pdev);

static const struct of_device_id mtk_smi_of_ids[] = {
	{ .compatible = "mediatek,mt8173-smi", .data = &smi_mt8173_priv, },
	{ .compatible = "mediatek,mt8163-smi", .data = &smi_mt8163_priv, },
	{ .compatible = "mediatek,mt8127-smi", .data = &smi_mt8127_priv, },
	{}
};

static struct platform_driver mtk_smi_driver = {
	.probe	= mtk_smi_probe,
	.remove = mtk_smi_remove,
	.driver	= {
		.name = "mtk-smi",
		.of_match_table = mtk_smi_of_ids,
	}
};

static int mtk_smi_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mtk_smi_common *smipriv;
	struct resource *res;
	const struct of_device_id *of_id;
	const struct mtk_smi_priv *priv;

	if (!dev->pm_domain)
		return -EPROBE_DEFER;

	smipriv = devm_kzalloc(dev, sizeof(*smipriv), GFP_KERNEL);
	if (!smipriv)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	smipriv->base = devm_ioremap_resource(dev, res);
	if (IS_ERR(smipriv->base))
		return PTR_ERR(smipriv->base);

	smipriv->clk_apb = devm_clk_get(dev, "apb");
	if (IS_ERR(smipriv->clk_apb))
		return PTR_ERR(smipriv->clk_apb);

	smipriv->clk_smi = devm_clk_get(dev, "smi");
	if (IS_ERR(smipriv->clk_smi))
		return PTR_ERR(smipriv->clk_smi);

	smi_data->smicommon = dev;
	smi_data->smi_common_base = (unsigned long)smipriv->base;

	of_id = of_match_node(mtk_smi_of_ids, dev->of_node);
	if (!of_id)
		return -EINVAL;

	priv = of_id->data;
	smi_data->smi_priv = priv;

	pm_runtime_enable(dev);
	dev_set_drvdata(dev, smipriv);
	return 0;
}

static int mtk_smi_remove(struct platform_device *pdev)
{
	pm_runtime_disable(&pdev->dev);
	return 0;
}
static int smi_suspend;
static int mtk_smi_larb_fb_suspend(void)
{
	int i;

	if (!smi_data || !smi_data->larb_nr) {
		SMIMSG("smi fb suspend, smi or smi larb did not probed\n");
		return 0;
	}

	for (i = 0; i < smi_data->larb_nr; i++) {
		mtk_smi_larb_clock_on(i, true);
		larb_reg_backup(i);
		mtk_smi_larb_clock_off(i, true);
	}
	smi_suspend = 1;
	SMIMSG("mtk_smi_larb fb suspended\n");
	return 0;
}

static int mtk_smi_larb_fb_resume(void)
{
	int i;

	if (!smi_suspend) {
		SMIMSG("resume without suspend\n");
		return 0;
	}

	if (!smi_data || !smi_data->larb_nr) {
		SMIMSG("smi fb resume, smi or smi larb did not probed\n");
		return 0;
	}

	for (i = 0; i < smi_data->larb_nr; i++) {
		mtk_smi_larb_clock_on(i, true);
		larb_reg_restore(i);
		mtk_smi_larb_clock_off(i, true);
	}

	smi_suspend = 0;
	SMIMSG("mtk_smi_larb fb resume\n");
	return 0;

}

static int mtk_smi_variant_event_notify(struct notifier_block *self,
				unsigned long action, void *data)
{
	if (action != FB_EARLY_EVENT_BLANK)
		return 0;

	{
		struct fb_event *event = data;
		int blank_mode = *((int *)event->data);

		switch (blank_mode) {
		case FB_BLANK_UNBLANK:
		case FB_BLANK_NORMAL:
			mtk_smi_larb_fb_resume();
			break;
		case FB_BLANK_VSYNC_SUSPEND:
		case FB_BLANK_HSYNC_SUSPEND:
			break;
		case FB_BLANK_POWERDOWN:
			mtk_smi_larb_fb_suspend();
			break;
		default:
			return -EINVAL;
		}
	}
	return 0;
}


static struct notifier_block mtk_smi_variant_event_notifier = {
	.notifier_call  = mtk_smi_variant_event_notify,
};

static int __init smi_init(void)
{
	int ret;

	smi_data = kzalloc(sizeof(*smi_data), GFP_KERNEL);
	if (smi_data == NULL) {
		SMIERR("Unable to allocate memory for smi driver");
		return -ENOMEM;
	}

	ret = platform_driver_register(&mtk_smi_driver);
	if (ret != 0) {
		pr_err("Failed to register SMI driver\n");
		return ret;
	}

	ret = platform_driver_register(&mtk_smi_larb_driver);
	if (ret != 0) {
		pr_err("Failed to register SMI-LARB driver\n");
		return ret;
	}

	ret = smi_dev_register();
	if (ret) {
		SMIMSG("register dev/smi failed\n");
		return ret;
	}

	memset(g_SMIInfo.pu4ConcurrencyTable, 0, SMI_BWC_SCEN_CNT * sizeof(unsigned int));
	spin_lock_init(&g_SMIInfo.SMI_lock);

	SMI_DBG_Init();
	fb_register_client(&mtk_smi_variant_event_notifier);
	SMIMSG("smi_init done\n");

	return 0;
}

static void __exit smi_exit(void)
{
	platform_driver_unregister(&mtk_smi_driver);
	platform_driver_unregister(&mtk_smi_larb_driver);
}

static int __init smi_init_late(void)
{
	/*init clk/mtcmos should be late while ccf */
	SMIMSG("smi_init_late-\n");

	smi_common_init();

	return 0;
}

static void smi_dumpCommonDebugMsg(void)
{
	unsigned long u4Base;

	/* SMI COMMON dump */
	SMIMSG("===SMI common reg dump===\n");

	u4Base = SMI_COMMON_EXT_BASE;
	SMIMSG("[0x200,0x204,0x208]=[0x%x,0x%x,0x%x]\n", M4U_ReadReg32(u4Base, 0x200),
	       M4U_ReadReg32(u4Base, 0x204), M4U_ReadReg32(u4Base, 0x208));
	SMIMSG("[0x20C,0x210,0x214]=[0x%x,0x%x,0x%x]\n", M4U_ReadReg32(u4Base, 0x20C),
	       M4U_ReadReg32(u4Base, 0x210), M4U_ReadReg32(u4Base, 0x214));
	if (smi_data->smi_priv->plat == MTK_PLAT_MT8173 || smi_data->smi_priv->plat == MTK_PLAT_MT8163) {
		SMIMSG("[0x220,0x230,0x234,0x238]=[0x%x,0x%x,0x%x,0x%x]\n", M4U_ReadReg32(u4Base, 0x220),
		       M4U_ReadReg32(u4Base, 0x230), M4U_ReadReg32(u4Base, 0x234), M4U_ReadReg32(u4Base,
												 0x238));
		SMIMSG("[0x400,0x404,0x408]=[0x%x,0x%x,0x%x]\n", M4U_ReadReg32(u4Base, 0x400),
		       M4U_ReadReg32(u4Base, 0x404), M4U_ReadReg32(u4Base, 0x408));

	} else if (smi_data->smi_priv->plat == MTK_PLAT_MT8127) {
		SMIMSG("[0x218,0x230,0x234,0x238]=[0x%x,0x%x,0x%x,0x%x]\n", M4U_ReadReg32(u4Base, 0x218),
		       M4U_ReadReg32(u4Base, 0x230), M4U_ReadReg32(u4Base, 0x234), M4U_ReadReg32(u4Base,
												 0x238));
		SMIMSG("[0x400,0x404,]=[0x%x,0x%x]\n", M4U_ReadReg32(u4Base, 0x400),
		       M4U_ReadReg32(u4Base, 0x404));
	}


	/* TBD: M4U should dump these, the offset of MT27 have been checked and same with the followings. */
/*
	For VA and PA check:
	0x1000C5C0 , 0x1000C5C4, 0x1000C5C8, 0x1000C5CC, 0x1000C5D0
	u4Base = SMI_COMMON_AO_BASE;
	SMIMSG("===SMI always on reg dump===\n");
	SMIMSG("[0x5C0,0x5C4,0x5C8]=[0x%x,0x%x,0x%x]\n" ,
		M4U_ReadReg32(u4Base , 0x5C0),M4U_ReadReg32(u4Base , 0x5C4),
		M4U_ReadReg32(u4Base , 0x5C8));
	SMIMSG("[0x5CC,0x5D0]=[0x%x,0x%x]\n" ,M4U_ReadReg32(u4Base , 0x5CC),
		M4U_ReadReg32(u4Base , 0x5D0));
*/
}

static void smi_dumpLarbDebugMsg(unsigned int u4Index)
{
	unsigned long u4Base;

	u4Base = get_larb_base_addr(u4Index);

	if (u4Base == SMI_ERROR_ADDR) {
		SMIMSG("Doesn't support reg dump for Larb%d\n", u4Index);
	} else {
		unsigned int u4Offset = 0;

		SMIMSG("===SMI LARB%d reg dump===\n", u4Index);

		if (smi_data->smi_priv->plat == MTK_PLAT_MT8173 || smi_data->smi_priv->plat == MTK_PLAT_MT8163) {
			SMIMSG("[0x0,0x8,0x10]=[0x%x,0x%x,0x%x]\n", M4U_ReadReg32(u4Base, 0x0),
			       M4U_ReadReg32(u4Base, 0x8), M4U_ReadReg32(u4Base, 0x10));
			SMIMSG("[0x24,0x50,0x60]=[0x%x,0x%x,0x%x]\n", M4U_ReadReg32(u4Base, 0x24),
			       M4U_ReadReg32(u4Base, 0x50), M4U_ReadReg32(u4Base, 0x60));
			SMIMSG("[0xa0,0xa4,0xa8]=[0x%x,0x%x,0x%x]\n", M4U_ReadReg32(u4Base, 0xa0),
			       M4U_ReadReg32(u4Base, 0xa4), M4U_ReadReg32(u4Base, 0xa8));
			SMIMSG("[0xac,0xb0,0xb4]=[0x%x,0x%x,0x%x]\n", M4U_ReadReg32(u4Base, 0xac),
			       M4U_ReadReg32(u4Base, 0xb0), M4U_ReadReg32(u4Base, 0xb4));
			SMIMSG("[0xb8,0xbc,0xc0]=[0x%x,0x%x,0x%x]\n", M4U_ReadReg32(u4Base, 0xb8),
			       M4U_ReadReg32(u4Base, 0xbc), M4U_ReadReg32(u4Base, 0xc0));
			SMIMSG("[0xc8,0xcc]=[0x%x,0x%x]\n", M4U_ReadReg32(u4Base, 0xc8),
			       M4U_ReadReg32(u4Base, 0xcc));
		} else if (smi_data->smi_priv->plat == MTK_PLAT_MT8127) {
			SMIMSG("[0x0,0x10,0x60]=[0x%x,0x%x,0x%x]\n", M4U_ReadReg32(u4Base, 0x0),
			       M4U_ReadReg32(u4Base, 0x10), M4U_ReadReg32(u4Base, 0x60));
			SMIMSG("[0x64,0x8c,0x450]=[0x%x,0x%x,0x%x]\n", M4U_ReadReg32(u4Base, 0x64),
			       M4U_ReadReg32(u4Base, 0x8c), M4U_ReadReg32(u4Base, 0x450));
			SMIMSG("[0x454,0x600,0x604]=[0x%x,0x%x,0x%x]\n", M4U_ReadReg32(u4Base, 0x454),
			       M4U_ReadReg32(u4Base, 0x600), M4U_ReadReg32(u4Base, 0x604));
			SMIMSG("[0x610,0x614]=[0x%x,0x%x]\n", M4U_ReadReg32(u4Base, 0x610),
			       M4U_ReadReg32(u4Base, 0x614));

		}

		for (u4Offset = 0x200; u4Offset <= 0x200 + smi_data->larb_nr * 4; u4Offset += 4)
			SMIMSG("[0x%x = 0x%x ]\n", u4Offset, M4U_ReadReg32(u4Base , u4Offset));
	}
}

static void smi_dump_format(unsigned long base, unsigned int from, unsigned int to)
{
	int i, j, left;
	unsigned int value[8];

	for (i = from; i <= to; i += 32) {
		for (j = 0; j < 8; j++)
			value[j] = M4U_ReadReg32(base, i + j * 4);

		SMIMSG2("%8x %x %x %x %x %x %x %x %x\n", i, value[0], value[1], value[2], value[3],
			value[4], value[5], value[6], value[7]);
	}

	left = ((from - to) / 4 + 1) % 8;

	if (left) {
		memset(value, 0, 8 * sizeof(unsigned int));

		for (j = 0; j < left; j++)
			value[j] = M4U_ReadReg32(base, i - 32 + j * 4);

		SMIMSG2("%8x %x %x %x %x %x %x %x %x\n", i - 32 + j * 4, value[0], value[1],
			value[2], value[3], value[4], value[5], value[6], value[7]);
	}
}

static void smi_dumpLarb(unsigned int index)
{
	unsigned long u4Base;

	u4Base = get_larb_base_addr(index);

	if (u4Base == SMI_ERROR_ADDR) {
		SMIMSG2("Doesn't support reg dump for Larb%d\n", index);

	} else {
		SMIMSG2("===SMI LARB%d reg dump base 0x%lx===\n", index, u4Base);

		smi_dump_format(u4Base, 0, 0x434);
		smi_dump_format(u4Base, 0xF00, 0xF0C);
	}
}

static void smi_dumpCommon(void)
{
	SMIMSG2("===SMI COMMON reg dump base 0x%lx===\n", SMI_COMMON_EXT_BASE);

	smi_dump_format(SMI_COMMON_EXT_BASE, 0x1A0, 0x418);
}

void smi_dumpDebugMsg(void)
{
	unsigned int u4Index;

	/* SMI COMMON dump */
	smi_dumpCommonDebugMsg();

	/* dump all SMI LARB */
	for (u4Index = 0; u4Index <= smi_data->larb_nr; u4Index++)
		smi_dumpLarbDebugMsg(u4Index);
}

int smi_debug_bus_hanging_detect(unsigned int larbs, int show_dump)
{
#ifdef CONFIG_MTK_SMI_EXT
	int i = 0;
	int dump_time = 0;
	int is_smi_issue = 0;
	int status_code = 0;
	/* Keep the dump result */
	unsigned char smi_common_busy_count = 0;
	/*volatile */ unsigned int reg_temp = 0;
	unsigned char smi_larb_busy_count[SMI_LARB_NR_MAX] = { 0 };
	unsigned char smi_larb_mmu_status[SMI_LARB_NR_MAX] = { 0 };

	/* dump resister and save resgister status */
	for (dump_time = 0; dump_time < 5; dump_time++) {
		unsigned int u4Index = 0;

		reg_temp = M4U_ReadReg32(SMI_COMMON_EXT_BASE, 0x400);
		if ((reg_temp & (1 << 30)) == 0) {
			/* smi common is busy */
			smi_common_busy_count++;
		}
		/* Dump smi common regs */
		if (show_dump != 0)
			smi_dumpCommonDebugMsg();

		for (u4Index = 0; u4Index <= smi_data->larb_nr; u4Index++) {
			unsigned long u4Base = get_larb_base_addr(u4Index);

			if (u4Base != SMI_ERROR_ADDR) {
				reg_temp = M4U_ReadReg32(u4Base, 0x0);
				if (reg_temp != 0) {
					/* Larb is busy */
					smi_larb_busy_count[u4Index]++;
				}
				smi_larb_mmu_status[u4Index] = M4U_ReadReg32(u4Base, 0xa0);
				if (show_dump != 0)
					smi_dumpLarbDebugMsg(u4Index);
			}
		}

	}

	/* Show the checked result */
	for (i = 0; i <= smi_data->larb_nr; i++) {	/* Check each larb */
		if (SMI_DGB_LARB_SELECT(larbs, i)) {
			/* larb i has been selected */
			/* Get status code */

			if (smi_larb_busy_count[i] == 5) {	/* The larb is always busy */
				if (smi_common_busy_count == 5) {	/* smi common is always busy */
					status_code = 1;
				} else if (smi_common_busy_count == 0) {	/* smi common is always idle */
					status_code = 2;
				} else {
					status_code = 5;	/* smi common is sometimes busy and idle */
				}
			} else if (smi_larb_busy_count[i] == 0) {	/* The larb is always idle */
				if (smi_common_busy_count == 5) {	/* smi common is always busy */
					status_code = 3;
				} else if (smi_common_busy_count == 0) {	/* smi common is always idle */
					status_code = 4;
				} else {
					status_code = 6;	/* smi common is sometimes busy and idle */
				}
			} else {	/* sometime the larb is busy */
				if (smi_common_busy_count == 5) {	/* smi common is always busy */
					status_code = 7;
				} else if (smi_common_busy_count == 0) {	/* smi common is always idle */
					status_code = 8;
				} else {
					status_code = 9;	/* smi common is sometimes busy and idle */
				}
			}

			/* Send the debug message according to the final result */
			switch (status_code) {
			case 1:
			case 3:
			case 5:
			case 7:
			case 8:
				SMIMSG
				    ("Larb%d Busy=%d/5, SMI Common Busy=%d/5, status=%d ==> Check engine's state first",
				     i, smi_larb_busy_count[i], smi_common_busy_count, status_code);
				SMIMSG
				    ("If the engine is waiting for Larb%ds' response, it needs SMI HW's check",
				     i);
				break;
			case 2:
				if (smi_larb_mmu_status[i] == 0) {
					SMIMSG("Larb%d Busy=%d/5, Common Busy=%d/5,status=%d=>Check engine state first",
					     i, smi_larb_busy_count[i], smi_common_busy_count,
					     status_code);
					SMIMSG("If the engine is waiting for Larb%ds' response,it needs SMI HW's check",
							i);
				} else {
					SMIMSG("Larb%d Busy=%d/5, Common Busy=%d/5, status=%d==>MMU port config error",
						i, smi_larb_busy_count[i], smi_common_busy_count,
						status_code);
					is_smi_issue = 1;
				}
				break;
			case 4:
			case 6:
			case 9:
				SMIMSG
				    ("Larb%d Busy=%d/5, SMI Common Busy=%d/5, status=%d ==> not SMI issue",
				     i, smi_larb_busy_count[i], smi_common_busy_count, status_code);
				break;
			default:
				SMIMSG
				    ("Larb%d Busy=%d/5, SMI Common Busy=%d/5, status=%d ==> status unknown",
				     i, smi_larb_busy_count[i], smi_common_busy_count, status_code);
				break;
			}
		}
	}

	return is_smi_issue;
#endif
	return 0;
}

#if IS_ENABLED(CONFIG_COMPAT)
/* 32 bits process ioctl support: */
/* This is prepared for the future extension since currently the sizes of 32 bits */
/* and 64 bits smi parameters are the same. */

typedef struct {
	compat_int_t scenario;
	compat_int_t b_on_off;	/* 0 : exit this scenario , 1 : enter this scenario */
} MTK_SMI_COMPAT_BWC_CONFIG;

typedef struct {
	compat_int_t property;
	compat_int_t value1;
	compat_int_t value2;
} MTK_SMI_COMPAT_BWC_INFO_SET;

typedef struct {
	compat_uint_t flag;	/* Reserved */
	compat_int_t concurrent_profile;
	compat_int_t sensor_size[2];
	compat_int_t video_record_size[2];
	compat_int_t display_size[2];
	compat_int_t tv_out_size[2];
	compat_int_t fps;
	compat_int_t video_encode_codec;
	compat_int_t video_decode_codec;
	compat_int_t hw_ovl_limit;
} MTK_SMI_COMPAT_BWC_MM_INFO;

#define COMPAT_MTK_IOC_SMI_BWC_CONFIG      MTK_IOW(24, MTK_SMI_COMPAT_BWC_CONFIG)
#define COMPAT_MTK_IOC_SMI_BWC_INFO_SET    MTK_IOWR(28, MTK_SMI_COMPAT_BWC_INFO_SET)
#define COMPAT_MTK_IOC_SMI_BWC_INFO_GET    MTK_IOWR(29, MTK_SMI_COMPAT_BWC_MM_INFO)

static int compat_get_smi_bwc_config_struct(MTK_SMI_COMPAT_BWC_CONFIG __user *data32,
					    MTK_SMI_BWC_CONFIG __user *data)
{

	compat_int_t i;
	int err;

	/* since the int sizes of 32 A32 and A64 are equal so we don't convert them actually here */
	err = get_user(i, &(data32->scenario));
	err |= put_user(i, &(data->scenario));
	err |= get_user(i, &(data32->b_on_off));
	err |= put_user(i, &(data->b_on_off));

	return err;
}

static int compat_get_smi_bwc_mm_info_set_struct(MTK_SMI_COMPAT_BWC_INFO_SET __user *data32,
						 MTK_SMI_BWC_INFO_SET __user *data)
{

	compat_int_t i;
	int err;

	/* since the int sizes of 32 A32 and A64 are equal so we don't convert them actually here */
	err = get_user(i, &(data32->property));
	err |= put_user(i, &(data->property));
	err |= get_user(i, &(data32->value1));
	err |= put_user(i, &(data->value1));
	err |= get_user(i, &(data32->value2));
	err |= put_user(i, &(data->value2));

	return err;
}

static int compat_get_smi_bwc_mm_info_struct(MTK_SMI_COMPAT_BWC_MM_INFO __user *data32,
					     MTK_SMI_BWC_MM_INFO __user *data)
{
	compat_uint_t u;
	compat_int_t i;
	compat_int_t p[2];
	int err;

	/* since the int sizes of 32 A32 and A64 are equal so we don't convert them actually here */
	err = get_user(u, &(data32->flag));
	err |= put_user(u, &(data->flag));
	err |= get_user(i, &(data32->concurrent_profile));
	err |= put_user(i, &(data->concurrent_profile));
	err |= copy_from_user(p, &(data32->sensor_size), sizeof(p));
	err |= copy_to_user(&(data->sensor_size), p, sizeof(p));
	err |= copy_from_user(p, &(data32->video_record_size), sizeof(p));
	err |= copy_to_user(&(data->video_record_size), p, sizeof(p));
	err |= copy_from_user(p, &(data32->display_size), sizeof(p));
	err |= copy_to_user(&(data->display_size), p, sizeof(p));
	err |= copy_from_user(p, &(data32->tv_out_size), sizeof(p));
	err |= copy_to_user(&(data->tv_out_size), p, sizeof(p));
	err |= get_user(i, &(data32->fps));
	err |= put_user(i, &(data->fps));
	err |= get_user(i, &(data32->video_encode_codec));
	err |= put_user(i, &(data->video_encode_codec));
	err |= get_user(i, &(data32->video_decode_codec));
	err |= put_user(i, &(data->video_decode_codec));
	err |= get_user(i, &(data32->hw_ovl_limit));
	err |= put_user(i, &(data->hw_ovl_limit));


	return err;
}

static int compat_put_smi_bwc_mm_info_struct(MTK_SMI_COMPAT_BWC_MM_INFO __user *data32,
					     MTK_SMI_BWC_MM_INFO __user *data)
{

	compat_uint_t u;
	compat_int_t i;
	compat_int_t p[2];
	int err;

	/* since the int sizes of 32 A32 and A64 are equal so we don't convert them actually here */
	err = get_user(u, &(data->flag));
	err |= put_user(u, &(data32->flag));
	err |= get_user(i, &(data->concurrent_profile));
	err |= put_user(i, &(data32->concurrent_profile));
	err |= copy_from_user(p, &(data->sensor_size), sizeof(p));
	err |= copy_to_user(&(data32->sensor_size), p, sizeof(p));
	err |= copy_from_user(p, &(data->video_record_size), sizeof(p));
	err |= copy_to_user(&(data32->video_record_size), p, sizeof(p));
	err |= copy_from_user(p, &(data->display_size), sizeof(p));
	err |= copy_to_user(&(data32->display_size), p, sizeof(p));
	err |= copy_from_user(p, &(data->tv_out_size), sizeof(p));
	err |= copy_to_user(&(data32->tv_out_size), p, sizeof(p));
	err |= get_user(i, &(data->fps));
	err |= put_user(i, &(data32->fps));
	err |= get_user(i, &(data->video_encode_codec));
	err |= put_user(i, &(data32->video_encode_codec));
	err |= get_user(i, &(data->video_decode_codec));
	err |= put_user(i, &(data32->video_decode_codec));
	err |= get_user(i, &(data->hw_ovl_limit));
	err |= put_user(i, &(data32->hw_ovl_limit));
	return err;
}

long MTK_SMI_COMPAT_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	long ret;

	if (!filp->f_op || !filp->f_op->unlocked_ioctl)
		return -ENOTTY;

	switch (cmd) {
	case COMPAT_MTK_IOC_SMI_BWC_CONFIG:
		{
			if (COMPAT_MTK_IOC_SMI_BWC_CONFIG == MTK_IOC_SMI_BWC_CONFIG) {
				SMIMSG("Optimized compct IOCTL: COMPAT_MTK_IOC_SMI_BWC_CONFIG");
				return filp->f_op->unlocked_ioctl(filp, cmd,
								  (unsigned long)compat_ptr(arg));
			} else {

				MTK_SMI_COMPAT_BWC_CONFIG __user *data32;
				MTK_SMI_BWC_CONFIG __user *data;
				int err;

				data32 = compat_ptr(arg);
				data = compat_alloc_user_space(sizeof(MTK_SMI_BWC_CONFIG));

				if (data == NULL)
					return -EFAULT;

				err = compat_get_smi_bwc_config_struct(data32, data);
				if (err)
					return err;

				ret = filp->f_op->unlocked_ioctl(filp, MTK_IOC_SMI_BWC_CONFIG,
								 (unsigned long)data);
				return ret;
			}
		}

	case COMPAT_MTK_IOC_SMI_BWC_INFO_SET:
		{

			if (COMPAT_MTK_IOC_SMI_BWC_INFO_SET == MTK_IOC_SMI_BWC_INFO_SET) {
				SMIMSG("Optimized compct IOCTL: COMPAT_MTK_IOC_SMI_BWC_INFO_SET");
				return filp->f_op->unlocked_ioctl(filp, cmd,
								  (unsigned long)compat_ptr(arg));
			} else {

				MTK_SMI_COMPAT_BWC_INFO_SET __user *data32;
				MTK_SMI_BWC_INFO_SET __user *data;
				int err;

				data32 = compat_ptr(arg);
				data = compat_alloc_user_space(sizeof(MTK_SMI_BWC_INFO_SET));
				if (data == NULL)
					return -EFAULT;

				err = compat_get_smi_bwc_mm_info_set_struct(data32, data);
				if (err)
					return err;

				return filp->f_op->unlocked_ioctl(filp, MTK_IOC_SMI_BWC_INFO_SET,
								  (unsigned long)data);
			}
		}
		break;

	case COMPAT_MTK_IOC_SMI_BWC_INFO_GET:
		{
			if (COMPAT_MTK_IOC_SMI_BWC_INFO_GET == MTK_IOC_SMI_BWC_INFO_GET) {
				SMIMSG("Optimized compct IOCTL: COMPAT_MTK_IOC_SMI_BWC_INFO_GET");
				return filp->f_op->unlocked_ioctl(filp, cmd,
								  (unsigned long)compat_ptr(arg));
			} else {
				MTK_SMI_COMPAT_BWC_MM_INFO __user *data32;
				MTK_SMI_BWC_MM_INFO __user *data;
				int err;

				data32 = compat_ptr(arg);
				data = compat_alloc_user_space(sizeof(MTK_SMI_BWC_MM_INFO));

				if (data == NULL)
					return -EFAULT;

				err = compat_get_smi_bwc_mm_info_struct(data32, data);
				if (err)
					return err;

				ret = filp->f_op->unlocked_ioctl(filp, MTK_IOC_SMI_BWC_INFO_GET,
								 (unsigned long)data);

				err = compat_put_smi_bwc_mm_info_struct(data32, data);

				if (err)
					return err;

				return ret;
			}
		}
		break;

	case MTK_IOC_SMI_DUMP_LARB:
	case MTK_IOC_SMI_DUMP_COMMON:
	case MTK_IOC_MMDVFS_CMD:

		return filp->f_op->unlocked_ioctl(filp, cmd, (unsigned long)compat_ptr(arg));
	default:
		return -ENOIOCTLCMD;
	}

}

#endif

module_init(smi_init);
module_exit(smi_exit);
late_initcall(smi_init_late);

module_param_named(debug_level, smi_debug_level, uint, S_IRUGO | S_IWUSR);
module_param_named(tuning_mode, smi_tuning_mode, uint, S_IRUGO | S_IWUSR);
module_param_named(wifi_disp_transaction, wifi_disp_transaction, uint, S_IRUGO | S_IWUSR);

MODULE_DESCRIPTION("MTK SMI driver");
MODULE_AUTHOR("Glory Hung<glory.hung@mediatek.com>");
MODULE_AUTHOR("Yong Wu<yong.wu@mediatek.com>");
MODULE_LICENSE("GPL");
