# frozen_string_literal: true
require "test/unit"
require "fiber"

class TestFiberCurrentRactor < Test::Unit::TestCase
  def setup
    omit unless defined? Ractor
  end

  def test_ractor_shareable
    assert_separately([], "#{<<~"begin;"}\n#{<<~'end;'}")
    begin;
      $VERBOSE = nil
      require "fiber"
      r = Ractor.new do
        Fiber.new do
          Fiber.current.class
        end.resume
      end
      assert_equal(Fiber, r.value)
    end;
  end
end

class TestFiberRactorScheduler < Test::Unit::TestCase
  def setup
    omit unless defined? Ractor
  end

  # Ractor::Port#receive in a non-blocking fiber must suspend only that fiber
  # via the fiber scheduler, so sibling fibers on the same thread keep running.
  def test_port_receive_from_sibling_fiber
    assert_separately([], "#{<<~"begin;"}\n#{<<~"end;"}")
    begin;
      $VERBOSE = nil
      require #{File.expand_path('scheduler', __dir__).dump}

      order = []
      value = nil

      thread = Thread.new do
        Fiber.set_scheduler Scheduler.new
        port = Ractor::Port.new

        Fiber.schedule do
          order << :receiving
          value = port.receive
          order << :received
        end

        Fiber.schedule do
          order << :sending
          port << :hello
          order << :sent
        end
      end
      thread.join

      assert_equal [:receiving, :sending, :sent, :received], order
      assert_equal :hello, value
    end;
  end

  def test_port_receive_from_other_ractor
    assert_separately([], "#{<<~"begin;"}\n#{<<~"end;"}")
    begin;
      $VERBOSE = nil
      require #{File.expand_path('scheduler', __dir__).dump}

      order = []
      value = nil

      thread = Thread.new do
        Fiber.set_scheduler Scheduler.new
        port = Ractor::Port.new

        Fiber.schedule do
          order << :receiving
          value = port.receive
          order << :received
        end

        # runs only if the receiving fiber suspended instead of blocking the thread
        Fiber.schedule do
          order << :spawning
          Ractor.new(port) {|p| p << :from_ractor }
        end
      end
      thread.join

      assert_equal [:receiving, :spawning, :received], order
      assert_equal :from_ractor, value
    end;
  end

  def test_ractor_value_from_fiber
    assert_separately([], "#{<<~"begin;"}\n#{<<~"end;"}")
    begin;
      $VERBOSE = nil
      require #{File.expand_path('scheduler', __dir__).dump}

      order = []
      value = nil

      thread = Thread.new do
        Fiber.set_scheduler Scheduler.new
        r = Ractor.new { Ractor.receive }

        Fiber.schedule do
          order << :awaiting
          value = r.value
          order << :got_value
        end

        Fiber.schedule do
          order << :sending
          r.send :ractor_result
        end
      end
      thread.join

      assert_equal [:awaiting, :sending, :got_value], order
      assert_equal :ractor_result, value
    end;
  end
end
