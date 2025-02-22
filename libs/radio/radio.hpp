#pragma once
#include "system.hpp"
#include "registers.hpp"
#include <functional>

namespace Radio
{
   enum eIrq : unsigned short
   {
      FifoAlmostFull = 1 << 12,
      RxDone = 1 << 13,
   };

   enum class eFskMode : unsigned char
   {
      Fsk1200 = 0,
      Ffsk1200_1200_1800,
      Ffsk1200_1200_2400,
      NoaaSame,
      ModesCount,
   };

   struct TFskModeBits
   {
      unsigned char u8TxModeBits;
      unsigned char u8RxBandWidthBits;
      unsigned char u8RxModeBits;
   };

   inline const TFskModeBits ModesBits[(int)eFskMode::ModesCount] =
   {// Tx mode    Rx badwitdh       Rx Mode
      {0b000,        0b000,         0b000},  // Fsk1200
      {0b001,        0b001,         0b111},  // Ffsk1200_1200_1800
      {0b011,        0b100,         0b100},  // Ffsk1200_1200_2400
      {0b101,        0b010,         0b000},  // NoaaSame
   };

   enum class eState : unsigned char
   {
      Idle,
      RxPending,
   };

   struct IRadioUser
   {
      virtual void RxDoneHandler(unsigned char u8DataLen, bool bCrcOk){};
   };

   template <const System::TOrgFunctions &Fw>
   class CBK4819
   {
   public:
      CBK4819() : State(eState::Idle), u16RxDataLen(0){};

      void SetFrequency(unsigned int u32FrequencyD10)
      {
         Fw.BK4819WriteFrequency(u32FrequencyD10);
      }

      unsigned int GetFrequency()
      {
         return (Fw.BK4819Read(0x39) << 16) | Fw.BK4819Read(0x38);
      }

      void SendSyncAirCopyMode72(unsigned char *p8Data)
      {
         Fw.BK4819ConfigureAndStartTxFsk();
         Fw.AirCopyFskSetup();
         SetFskMode(eFskMode::Fsk1200);
         Fw.AirCopy72(p8Data);
         Fw.BK4819SetGpio(1, false);
      }

      void DisablePa()
      {
         Fw.BK4819Write(0x30, Fw.BK4819Read(0x30) & ~0b1010);
      }

      void SetFskMode(eFskMode Mode)
      {
         auto const& ModeParams = ModesBits[(int)Mode];
         auto Reg58 = Fw.BK4819Read(0x58);
         Reg58 &= ~((0b111 << 1) | (0b111 << 10) | (0b111 << 13));
         Reg58 |=   (ModeParams.u8RxBandWidthBits << 1) 
                  | (ModeParams.u8RxModeBits << 10)
                  | (ModeParams.u8TxModeBits << 13);
         Fw.BK4819Write(0x58, 0);
         Fw.BK4819Write(0x58, Reg58);
      }

      void FixIrqEnRegister() // original firmware overrides IRQ_EN reg, so we need to reenable it
      {
         auto const OldIrqEnReg = Fw.BK4819Read(0x3F);
         if((OldIrqEnReg & (eIrq::FifoAlmostFull | eIrq::RxDone)) != 
            (eIrq::FifoAlmostFull | eIrq::RxDone))
         {
            Fw.BK4819Write(0x3F, OldIrqEnReg | eIrq::FifoAlmostFull | eIrq::RxDone);
         }
      }

      void RecieveAsyncAirCopyMode(unsigned char *p8Data, unsigned char u8DataLen, IRadioUser *pUser)
      {
         if (!p8Data || !u8DataLen)
         {
            return;
         }

         pRadioUser = pUser;
         p8RxBuff = p8Data;
         u8RxBuffSize = u8DataLen;
         u16RxDataLen = 0;

         Fw.AirCopyFskSetup();
         SetFskMode(eFskMode::Fsk1200);
         Fw.BK4819ConfigureAndStartRxFsk();
         State = eState::RxPending;
      }

      void DisableFskModem()
      {
         auto const FskSettings = Fw.BK4819Read(0x58);
         Fw.BK4819Write(0x58, FskSettings & ~1);
      }

      void ClearRxFifoBuff()
      {
         auto const Reg59 = Fw.BK4819Read(0x59);
         Fw.BK4819Write(0x59, 1 << 14);
         Fw.BK4819Write(0x59, Reg59);
      }

      unsigned short GetIrqReg()
      {
         Fw.BK4819Write(0x2, 0);
         return Fw.BK4819Read(0x2);
      }

      bool CheckCrc()
      {
         return Fw.BK4819Read(0x0B) & (1 << 4);
      }

      bool IsLockedByOrgFw()
      {
         return !(GPIOC->DATA & 0b1);
      }

      unsigned short u16DebugIrq;

      void HandleRxDone()
      {
         ClearRxFifoBuff();
         DisableFskModem();
         State = eState::Idle;
         if (pRadioUser)
         {
            pRadioUser->RxDoneHandler(u16RxDataLen, CheckCrc());
         }
      }

      void InterruptHandler()
      {
         if (IsLockedByOrgFw())
         {
            return;
         }


         if (State == eState::RxPending)
         {
            FixIrqEnRegister();
            if (!(Fw.BK4819Read(0x0C) & 1)) // irq request indicator
            {
               return;
            }

            auto const IrqReg = GetIrqReg();

            if (IrqReg & eIrq::RxDone)
            {
               // HandleRxDone();
            }

            if (IrqReg & eIrq::FifoAlmostFull)
            {
               HandleFifoAlmostFull();
            }
         }
      }

      eState State;
      unsigned short u16RxDataLen;

   private:
      void HandleFifoAlmostFull()
      {
         for (unsigned char i = 0; i < 4; i++)
         {
            auto const RxData = Fw.BK4819Read(0x5F);
            if (p8RxBuff && u16RxDataLen < u8RxBuffSize - 2)
            {
               memcpy(p8RxBuff + u16RxDataLen, &RxData, 2);
            }

            u16RxDataLen += 2;
         }

         if (u16RxDataLen >= u8RxBuffSize)
         {
            State = eState::Idle;
            if (pRadioUser)
            {
               pRadioUser->RxDoneHandler(u8RxBuffSize, CheckCrc());
            }
         }
      }

      IRadioUser *pRadioUser;
      unsigned char *p8RxBuff;
      unsigned char u8RxBuffSize;
   };
}