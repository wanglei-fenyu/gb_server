log.Info("load script ...")

local function test_msg_pack()
    local bin = msgpack.pack(19, "aaa", 100)
    local a, b, c = msgpack.unpack(bin)
    log.Info(""..a)
    log.Info(""..b)
    log.Info(""..c)
end
test_msg_pack()

local function test_timer()
    -- Test 1: one-shot steady timer
    local oneshot_fired = false
    timer.Register(100, function()
        oneshot_fired = true
        log.Info("[TimerTest] one-shot fired")
    end, false)

    -- Test 2: looping timer + cancel after 3 ticks
    local loop_count = 0
    timer.Register(50, function(timer_id)
        loop_count = loop_count + 1
        log.Info("[TimerTest] loop tick " .. loop_count)
        if loop_count >= 3 then
            timer.UnRegister(timer_id)
            log.Info("[TimerTest] loop cancelled")
        end
    end, true)

    -- Test 3: system timer
    timer.RegisterSystem(120, function()
        log.Info("[TimerTest] system timer fired")
    end, false)

    -- Test 4: cancel before fire
    local cancel_id = timer.Register(5000, function()
        log.Error("[TimerTest] FAIL: cancelled timer fired!")
    end, false)
    timer.UnRegister(cancel_id)
    log.Info("[TimerTest] cancel_id=" .. tostring(cancel_id))

    -- Check results after timers should have fired
    timer.Register(400, function()
        if oneshot_fired then
            log.Info("[TimerTest] RESULT: one-shot PASS")
        else
            log.Error("[TimerTest] RESULT: one-shot FAIL")
        end
        if loop_count >= 3 then
            log.Info("[TimerTest] RESULT: loop PASS")
        else
            log.Error("[TimerTest] RESULT: loop FAIL (count=" .. loop_count .. ")")
        end
        if cancel_id ~= nil then
            log.Info("[TimerTest] RESULT: cancel PASS")
        else
            log.Error("[TimerTest] RESULT: cancel FAIL - id is nil")
        end
        log.Info("[TimerTest] all done")
    end, false)
end

timer.Register(200, function()
    log.Info("===== TimerTest BEGIN =====")
    test_timer()
end, false)


function world()
    log.Error("xxxxxxxxx")
end



function lua_rpc_test(reply)
    log.Warning("lua_rpc_test")
end

function lua_rpc_test_args(reply,a)
    log.Warning("lua_rpc_test_args"..":"..a)
    reply:Invoke(a..a)        
end

net.Register("lua_rpc_test",lua_rpc_test)
net.Register("lua_rpc_test_args",lua_rpc_test_args)



function hello(session,message)
    log.Error("xxxxxxxxxxxxxxxxxxxxxx")
    log.Error(message:msg())
    log.Info("msg:"..message:msg().." index:"..message:index())
    log.Warning(message:index().."")
    --net.Send(session,1,2,"TestMsg",message)
end
net.Listen(1,hello,"TestMsg")
