// index.ts — CAINE ESP 控制臺
Page({
  data: {
    statusBarHeight: 44,
    humidity: 47,
    temperature: 23,
    light: 77,
    onlineMark: '√',
    deviceOnline: true,
  },

  onLoad() {
    const sys = wx.getSystemInfoSync()
    const bar = sys.statusBarHeight || 44
    this.setData({
      statusBarHeight: bar,
    })
  },

  onArrowTap(e: WechatMiniprogram.TouchEvent) {
    const dir = (e.currentTarget.dataset as { dir?: string }).dir
    if (!dir) return
    wx.showToast({
      title: `方向：${dir}`,
      icon: 'none',
      duration: 800,
    })
    // 在此處發送 MQTT / HTTP 指令到 ESP 設備
    console.log('[CAINE ESP] direction:', dir)
  },
})
