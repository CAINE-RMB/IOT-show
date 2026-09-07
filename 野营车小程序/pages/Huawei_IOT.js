// pages/Huawei_IOT.js
Page({

    /**
     * 页面的初始数据
     */
    data: {
        statusBarHeight: 44,
        result: '等待取得 Token',
        humidityLine: '--',
        temperatureLine: '--',
        lightLine: '--',
        onlineMark: '—',
    },
    /**
     * 获取token按钮按下：
     */
    touchBtn_gettoken:function()
    {
        console.log("获取token按钮按下");
        this.setData({ result: '取得 Token…' });
        this.gettoken();
    },
    /**
     * 获取设备影子按钮按下：
     */
    touchBtn_getshadow:function()
    {
        console.log("获取设备影子按钮按下");
        this.setData({ result: '取得設備影子…' });
        this.getshadow();
    },
    /**
     * 方向鍵下發（dataset.dir：up / down / left / right）
     */
    onArrowTap: function (e) {
        var dir = e.currentTarget.dataset.dir;
        if (!dir) return;
        console.log('[Huawei IoT] direction:', dir);
        this.setData({ result: '下發方向：' + dir + '…' });
        this.sendDeviceCommand({ dir: dir });
    },
    /**
     * 获取token
     */
    gettoken:function(){
        console.log("开始获取。。。");//打印完整消息
        var that=this;  //这个很重要，在下面的回调函数中由于异步问题不能有效修改变量，需要用that获取
        wx.request({
            url: 'https://iam.cn-north-4.myhuaweicloud.com/v3/auth/tokens',
            data:`{
    "auth": {
        "identity": {
            "methods": ["password"],
            "password": {
                "user": {
                    "domain": {
                        "name": "aa031002"
                    },
                    "name": "CAINE",
                    "password": "031002Aa"
                }
            }
        },
        "scope": {
            "project": {
                "name": "cn-north-4"
            }
        }
    }
}`,


            method: 'POST', // OPTIONS, GET, HEAD, POST, PUT, DELETE, TRACE, CONNECT
            header: {'Content-Type': 'application/json' }, // 请求的 header 
            success: function(res){// success
              // success
                console.log("获取token请求完成");//打印完整消息
                console.log("完整响应：", res);//打印完整消息
                if(res.statusCode === 201) {
                    console.log("获取token成功");//打印完整消息
                    var token='';
                    token=JSON.stringify(res.header['X-Subject-Token']);//解析消息头的token
                    token=token.replaceAll("\"", "");
                    console.log("获取token=\n"+token);//打印token
                    wx.setStorageSync('token',token);//把token写到缓存中,以便可以随时随地调用
                    that.setData({ result: '取得 Token 成功' });
                } else {
                    console.log("获取token失败，状态码：" + res.statusCode);//打印完整消息
                    console.log("错误详情：", res.data);//打印错误详情
                    that.setData({ result: '取得 Token 失敗，請檢查認證資訊' });
                }
            },
            fail:function(){
                // fail
                console.log("获取token失败");//打印完整消息
                that.setData({ result: '取得 Token 失敗，網路錯誤' });
            },
            complete: function() {
                // complete
                console.log("获取token完成");//打印完整消息
            } 
        });
    },
    /**
     * 获取设备影子
     */
    getshadow:function(){
        console.log("开始获取影子");//打印完整消息
        var that=this;  //这个很重要，在下面的回调函数中由于异步问题不能有效修改变量，需要用that获取
        var token=wx.getStorageSync('token');//读缓存中保存的token
        console.log("我的token:"+token);//打印完整消息
        if(!token){
            console.log("token不存在，请先获取token");//打印完整消息
            that.setData({ result: '尚無 Token，請先取得 Token' });
            return;
        }
        wx.request({
            url: 'https://4ce16929b3.st1.iotda-app.cn-north-4.myhuaweicloud.com:443/v5/iot/3856a93afb624a4cb1ce416f97fcccb2/devices/69a8f163cbb0cf6bb943fca5_20260305/shadow',
            data:'',
            method: 'GET', // OPTIONS, GET, HEAD, POST, PUT, DELETE, TRACE, CONNECT
            header: {'Content-Type': 'application/json','X-Auth-Token':token }, //请求的header 
            success: function(res){// success
            // success
                console.log(res);//打印完整消息
                if(res.statusCode === 200) {
                    var shadow=JSON.stringify(res.data.shadow[0].reported.properties);
                    console.log('设备影子数据：'+shadow);
                    //以下根据自己的设备属性进行解析
                    //设备影子属性：temperature（温度）, humidity（湿度）
                    var temperature = res.data.shadow[0].reported.properties.temperature;
                    var humidity = res.data.shadow[0].reported.properties.humidity;
                    
                    // 检查属性是否存在
                    if (temperature !== undefined && humidity !== undefined) {
                        console.log('温度='+temperature+'℃');
                        console.log('湿度='+humidity+'%');
                        var props = res.data.shadow[0].reported.properties;
                        var lightVal = props.light !== undefined ? props.light : props.illuminance;
                        var lightLine = '--';
                        if (lightVal !== undefined && lightVal !== null) {
                            var ln = Number(lightVal);
                            lightLine = (isNaN(ln) ? String(lightVal) : ln.toFixed(2)) + '%';
                        }
                        that.setData({
                            result: '溫度 ' + temperature + '°C，濕度 ' + humidity + '%',
                            temperatureLine: String(temperature) + '°C',
                            humidityLine: String(humidity) + '%',
                            lightLine: lightLine,
                            onlineMark: '√',
                        });
                    } else {
                        console.log('设备影子中缺少温度或湿度属性');
                        that.setData({ result: '設備影子缺少溫度或濕度屬性', onlineMark: '—' });
                    }
                } else {
                    console.log("获取影子失败，状态码：" + res.statusCode);//打印完整消息
                    that.setData({ result: '取得影子失敗，請檢查 Token 是否有效', onlineMark: '—' });
                }
            },
            fail:function(){
                // fail
                console.log("获取影子失败");//打印完整消息
                console.log("请先获取token");//打印完整消息
                that.setData({ result: '取得影子失敗，網路錯誤', onlineMark: '—' });
            },
            complete: function() {
                // complete
                console.log("获取影子完成");//打印完整消息
            } 
        });
    },
    /**
     * 設備命令下發（paras 由業務定義，方向鍵傳 { dir: 'up'|'down'|'left'|'right' }）
     * 若設備仍使用舊版 paras（如 led），請在雲端或固件中適配 dir 欄位。
     */
    sendDeviceCommand: function (paras) {
        console.log('下發命令 paras:', paras);
        var that = this;
        var token = wx.getStorageSync('token');
        console.log('我的token:' + token);
        if (!token) {
            console.log('token不存在，请先获取token');
            that.setData({ result: '尚無 Token，請先取得 Token' });
            return;
        }
        var body = JSON.stringify({
            service_id: 'Dev_data',
            command_name: 'Control',
            paras: paras
        });
        wx.request({
            url: 'https://4ce16929b3.st1.iotda-app.cn-north-4.myhuaweicloud.com:443/v5/iot/3856a93afb624a4cb1ce416f97fcccb2/devices/69a8f163cbb0cf6bb943fca5_20260305/commands',
            data: body,
            method: 'POST',
            header: { 'Content-Type': 'application/json', 'X-Auth-Token': token },
            success: function (res) {
                console.log('下發命令請求完成', res);
                if (res.statusCode === 200) {
                    var hint = paras.dir ? ('方向「' + paras.dir + '」已下發') : '命令已下發';
                    that.setData({ result: hint });
                } else {
                    console.log('命令下发失败，状态码：' + res.statusCode);
                    that.setData({ result: '命令下发成功' });
                }
            },
            fail: function () {
                console.log('命令下发失败');
                that.setData({ result: '命令下發失敗，網路錯誤' });
            },
            complete: function () {
                console.log('命令下发完成');
            }
        });
    },

      
    /**
     * 生命周期函数--监听页面加载
     */
    onLoad(options) {
        var sys = wx.getSystemInfoSync();
        var bar = sys.statusBarHeight || 44;
        this.setData({ statusBarHeight: bar });
    },

    /**
     * 生命周期函数--监听页面初次渲染完成
     */
    onReady() {

    },

    /**
     * 生命周期函数--监听页面显示
     */
    onShow() {

    },

    /**
     * 生命周期函数--监听页面隐藏
     */
    onHide() {

    },

    /**
     * 生命周期函数--监听页面卸载
     */
    onUnload() {

    },

    /**
     * 页面相关事件处理函数--监听用户下拉动作
     */
    onPullDownRefresh() {

    },

    /**
     * 页面上拉触底事件的处理函数
     */
    onReachBottom() {

    },

    /**
     * 用户点击右上角分享
     */
    onShareAppMessage() {

    }
})