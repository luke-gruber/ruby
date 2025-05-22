# frozen_string_literal: true
require "test/unit"
require "fiber"

class TestFiberCurrentRactor < Test::Unit::TestCase
  def setup
    omit unless defined? Ractor
  end

  def test_ractor_shareable
    assert_ractor "#{<<~"begin;"}\n#{<<~'end;'}", require: "fiber"
    begin;
      r = Ractor.new do
        Fiber.new do
          Fiber.current.class
        end.resume
      end
      assert_equal(Fiber, r.take)
    end;
  end

  def test_ractor_take_before_ractor_finished_in_fiber_scheduler_context
    assert_ractor("#{<<~"begin;"}\n#{<<~'end;'}", require: "fiber", require_relative: "scheduler")
    begin;
      scheduler = Scheduler.new
      Fiber.set_scheduler scheduler
      class << scheduler
        attr_reader :test_blockers
        def block(blocker, timeout=nil)
          (@test_blockers ||= []) << [blocker, timeout]
          super
        end
      end
      # in f1 (main fiber)
      ordering = []
      blocked_thread = nil
      Fiber.schedule do
        # in f2
        r = Ractor.new do
          # in f3
          sleep 0.5
        end
        ordering << "f2 before take"
        # Calling `r.take` should schedule us away from f2 back to f1. In f1, we end the script
        # and then Scheduler#run is called, which blocks on IO.select. When the ractor is finished,
        # it resumes fiber f2.
        blocked_thread = Thread.current
        r.take
        ordering << "f2 after take"
      end
      ordering << "f1 thread finish"
      expected_ordering = ["f2 before take", "f1 thread finish", "f2 after take"]
      at_exit do
        assert_equal expected_ordering, ordering
        assert_equal 1, scheduler.test_blockers.size
        assert scheduler.test_blockers.first[0].is_a?(Thread) # the blocked thread that called take
        assert_equal blocked_thread, scheduler.test_blockers.first[0]
        assert_equal nil, scheduler.test_blockers.first[1] # take is called without a timeout
      end
    end;
  end

  def test_ractor_take_after_ractor_finished_in_fiber_scheduler_context
    assert_ractor("#{<<~"begin;"}\n#{<<~'end;'}", require: "fiber", require_relative: "scheduler")
    begin;
      scheduler = Scheduler.new
      class << scheduler
        attr_reader :test_blockers
        def block(blocker, timeout=nil)
          (@test_blockers ||= []) << [blocker, timeout]
          super
        end
      end
      Fiber.set_scheduler scheduler
      # in f1 (main fiber)
      Fiber.schedule do
        # in f2
        r = Ractor.new do
          # in f3
          :done
        end
        sleep 0.5 # give time for ractor to finish
        # Calling `r.take` here should not block because the ractor is already done yielding its value
        r.take
      end
      at_exit do
        assert_equal 1, scheduler.test_blockers.size
        assert_equal [:sleep, 0.5], scheduler.test_blockers.first # sleep in the fiber scheduler blocked, but not `r.take`
      end
    end;
  end

  def test_ractor_take_in_non_fiber_scheduler_context
    assert_ractor("#{<<~"begin;"}\n#{<<~'end;'}", require: "fiber", require_relative: "scheduler")
    begin;
      scheduler = Scheduler.new
      class << scheduler
        attr_reader :test_blockers
        def block(blocker, timeout=nil)
          (@test_blockers ||= []) << [blocker, timeout]
          super
        end
      end
      Fiber.set_scheduler scheduler
      is_blocking = nil
      Fiber.new(blocking: true) do
        is_blocking = Fiber.current.blocking?
        r = Ractor.new do
          sleep 0.5
          :done
        end
        # Calling `r.take` here should block, NOT switch fibers (we're not in Fiber.schedule block AKA fiber scheduler context)
        r.take
      end.transfer
      at_exit do
        assert_equal true, is_blocking
        assert_equal nil, scheduler.test_blockers
      end
    end;
  end
end
