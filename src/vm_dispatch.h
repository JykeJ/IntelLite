#pragma once

// The shared Papyrus VM dispatch helpers every IntelLite module needs. Internal
// to this plugin (not from the vendor headers): movement, the SkyrimNet bridge
// and the task engine each dispatch static Papyrus calls through the VM.
//
// GAME THREAD ONLY. Args are always a fresh MakeFunctionArguments block (the VM
// takes ownership). A default-constructed (null) callback smart pointer is
// deliberately avoided; the no-op callback below is refcounted by the VM.

#include <RE/Skyrim.h>

namespace intellite::vm
{
	class NoopStackCallback : public RE::BSScript::IStackCallbackFunctor
	{
	public:
		void operator()(RE::BSScript::Variable) override {}
		void SetObject(const RE::BSTSmartPointer<RE::BSScript::Object>&) override {}
	};

	// A stack callback that schedules Actor::EvaluatePackage on the game thread
	// once the VM finishes executing a non-tasklet static call (such as
	// ActorUtil.AddPackageOverride / RemovePackageOverride), so package
	// evaluation runs after PapyrusUtil has mutated its override table rather
	// than only before the queued VM stack executes.
	class EvalPackageStackCallback : public RE::BSScript::IStackCallbackFunctor
	{
	public:
		EvalPackageStackCallback(RE::FormID a_actorFormID, bool a_resetAI) :
			actorFormID(a_actorFormID),
			resetAI(a_resetAI)
		{}

		void operator()(RE::BSScript::Variable) override
		{
			const auto id = actorFormID;
			const bool reset = resetAI;
			if (auto* taskInterface = SKSE::GetTaskInterface()) {
				taskInterface->AddTask([id, reset]() {
					if (auto* actor = RE::TESForm::LookupByID<RE::Actor>(id)) {
						if (!actor->IsDead()) {
							actor->EvaluatePackage(true, reset);
						}
					}
				});
			}
		}

		void SetObject(const RE::BSTSmartPointer<RE::BSScript::Object>&) override {}

	private:
		RE::FormID actorFormID = 0;
		bool       resetAI = false;
	};

	inline bool DispatchStatic(const char* a_class, const char* a_fn,
		RE::BSScript::IFunctionArguments* a_args)
	{
		auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
		if (!vm) {
			delete a_args;
			return false;
		}
		RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback{ new NoopStackCallback() };
		RE::BSFixedString className(a_class);
		RE::BSFixedString fnName(a_fn);
		return vm->DispatchStaticCall(className, fnName, a_args, callback);
	}

	inline bool DispatchStaticWithEval(const char* a_class, const char* a_fn,
		RE::BSScript::IFunctionArguments* a_args, RE::FormID a_actorFormID, bool a_resetAI)
	{
		auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
		if (!vm) {
			delete a_args;
			return false;
		}
		RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback{
			new EvalPackageStackCallback(a_actorFormID, a_resetAI)
		};
		RE::BSFixedString className(a_class);
		RE::BSFixedString fnName(a_fn);
		return vm->DispatchStaticCall(className, fnName, a_args, callback);
	}
}
