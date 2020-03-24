//============================================================================
//
//   SSSS    tt          lll  lll
//  SS  SS   tt           ll   ll
//  SS     tttttt  eeee   ll   ll   aaaa
//   SSSS    tt   ee  ee  ll   ll      aa
//      SS   tt   eeeeee  ll   ll   aaaaa  --  "An Atari 2600 VCS Emulator"
//  SS  SS   tt   ee      ll   ll  aa  aa
//   SSSS     ttt  eeeee llll llll  aaaaa
//
// Copyright (c) 1995-2020 by Bradford W. Mott, Stephen Anthony
// and the Stella Team
//
// See the file "License.txt" for information on usage and redistribution of
// this file, and for a DISCLAIMER OF ALL WARRANTIES.
//============================================================================

#include "System.hxx"
#include "CartMNetwork.hxx"

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
CartridgeMNetwork::CartridgeMNetwork(const ByteBuffer& image, size_t size,
                                     const string& md5, const Settings& settings)
  : Cartridge(settings, md5),
    mySize(size)
{
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void CartridgeMNetwork::initialize(const ByteBuffer& image, size_t size)
{
  // Allocate array for the ROM image
  myImage = make_unique<uInt8[]>(size);

  // Copy the ROM image into my buffer
  std::copy_n(image.get(), std::min<size_t>(romSize(), size), myImage.get());
  createCodeAccessBase(romSize() + myRAM.size());

  myRAMSlice = bankCount() - 1;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void CartridgeMNetwork::reset()
{
  initializeRAM(myRAM.data(), myRAM.size());

  initializeStartBank(0);
  uInt32 ramBank = randomStartBank() ?
    mySystem->randGenerator().next() % 4 : 0;

  // Install some default banks for the RAM and first segment
  bankRAM(ramBank);
  bank(startBank());

  myBankChanged = true;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void CartridgeMNetwork::setAccess(uInt16 addrFrom, uInt16 size,
    uInt16 directOffset, uInt8* directData, uInt16 codeOffset,
    System::PageAccessType type, uInt16 addrMask)
{
  if(addrMask == 0)
    addrMask = size - 1;
  System::PageAccess access(this, type);

  for(uInt16 addr = addrFrom; addr < addrFrom + size; addr += System::PAGE_SIZE)
  {
    if(type == System::PageAccessType::READ)
      access.directPeekBase = &directData[directOffset + (addr & addrMask)];
    else if(type == System::PageAccessType::WRITE)  // all RAM writes mapped to ::poke()
      access.directPokeBase = nullptr;
    access.codeAccessBase = &myCodeAccessBase[codeOffset + (addr & addrMask)];
    mySystem->setPageAccess(addr, access);
  }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void CartridgeMNetwork::install(System& system)
{
  mySystem = &system;

  System::PageAccess access(this, System::PageAccessType::READ);

  // Set the page accessing methods for the hot spots
  for(uInt16 addr = (0x1FE0 & ~System::PAGE_MASK); addr < 0x2000;
      addr += System::PAGE_SIZE)
  {
    access.codeAccessBase = &myCodeAccessBase[0x1fc0];
    mySystem->setPageAccess(addr, access);
  }
  /*setAccess(0x1FE0 & ~System::PAGE_MASK, System::PAGE_SIZE,
            0, nullptr, 0x1fc0, System::PA_NONE, 0x1fc0);*/

  // Setup the second segment to always point to the last ROM slice
  setAccess(0x1A00, 0x1FE0U & (~System::PAGE_MASK - 0x1A00),
            myRAMSlice * BANK_SIZE, myImage.get(), myRAMSlice * BANK_SIZE, System::PageAccessType::READ, BANK_SIZE - 1);
  myCurrentSlice[1] = myRAMSlice;

  // Install some default banks for the RAM and first segment
  bankRAM(0);
  bank(startBank());
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
uInt8 CartridgeMNetwork::peek(uInt16 address)
{
  uInt16 peekAddress = address;
  address &= 0x0FFF;

  // Switch banks if necessary
  checkSwitchBank(address);

  if((myCurrentSlice[0] == myRAMSlice) && (address < BANK_SIZE / 2))
  {
    // Reading from the 1K write port @ $1000 triggers an unwanted write
    return peekRAM(myRAM[address & (BANK_SIZE / 2 - 1)], peekAddress);
  }
  else if((address >= 0x0800) && (address <= 0x08FF))
  {
    // Reading from the 256B write port @ $1800 triggers an unwanted write
    return peekRAM(myRAM[0x0400 + (myCurrentRAM << 8) + (address & 0x00FF)], peekAddress);
  }
  else
    return myImage[(myCurrentSlice[address >> 11] << 11) + (address & (BANK_SIZE - 1))];
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
bool CartridgeMNetwork::poke(uInt16 address, uInt8 value)
{
  uInt16 pokeAddress = address;
  address &= 0x0FFF;

  // Switch banks if necessary
  checkSwitchBank(address);

  // All RAM writes are mapped here
  if((myCurrentSlice[0] == myRAMSlice) && (address < BANK_SIZE / 2))
  {
    // RAM slices
    if(!(address & 0x0400))
    {
      pokeRAM(myRAM[address & (BANK_SIZE / 2 - 1)], pokeAddress, value);
      return true;
    }
    else
    {
      // Writing to the read port should be ignored, but trigger a break if option enabled
      uInt8 dummy;

      pokeRAM(dummy, pokeAddress, value);
      myRamWriteAccess = pokeAddress;
      return false;
    }
  }
  else
  {
    // fixed 256 bytes of RAM
    if((address >= 0x0800) && (address <= 0x09FF))
    {
      if(!(address & 0x100))
      {
        pokeRAM(myRAM[0x0400 + (myCurrentRAM << 8) + (address & 0x00FF)], pokeAddress, value);
        return true;
      }
      else
      {
        // Writing to the read port should be ignored, but trigger a break if option enabled
        uInt8 dummy;

        pokeRAM(dummy, pokeAddress, value);
        myRamWriteAccess = pokeAddress;
        return false;
      }
    }
  }

  return false;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void CartridgeMNetwork::bankRAM(uInt16 bank)
{
  if(bankLocked()) return;

  // Remember what bank we're in
  myCurrentRAM = bank;
  uInt16 offset = bank << 8; // * RAM_SLICE_SIZE (256)

  // Setup the page access methods for the current bank
  // Set the page accessing method for the 256 bytes of RAM reading pages
  setAccess(0x1800, 0x100, 0x0400 + offset, myRAM.data(), romSize() + BANK_SIZE / 2, System::PageAccessType::WRITE);
  // Set the page accessing method for the 256 bytes of RAM reading pages
  setAccess(0x1900, 0x100, 0x0400 + offset, myRAM.data(), romSize() + BANK_SIZE / 2, System::PageAccessType::READ);

  myBankChanged = true;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
bool CartridgeMNetwork::bank(uInt16 slice)
{
  if(bankLocked()) return false;

  // Remember what bank we're in
  myCurrentSlice[0] = slice;

  // Setup the page access methods for the current bank
  if(slice != myRAMSlice)
  {
    uInt16 offset = slice << 11; // * BANK_SIZE (2048)

    // Map ROM image into first segment
    setAccess(0x1000, BANK_SIZE, offset, myImage.get(), offset, System::PageAccessType::READ);
  }
  else
  {
    // Set the page accessing method for the 1K slice of RAM writing pages
    setAccess(0x1000,                 BANK_SIZE / 2, 0, myRAM.data(), romSize(), System::PageAccessType::WRITE);
    // Set the page accessing method for the 1K slice of RAM reading pages
    setAccess(0x1000 + BANK_SIZE / 2, BANK_SIZE / 2, 0, myRAM.data(), romSize(), System::PageAccessType::READ);
  }
  return myBankChanged = true;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
uInt16 CartridgeMNetwork::getBank(uInt16 address) const
{
  return myCurrentSlice[(address & 0xFFF) >> 11]; // 2K slices
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
bool CartridgeMNetwork::patch(uInt16 address, uInt8 value)
{
  address = address & 0x0FFF;

  if(address < 0x0800)
  {
    if(myCurrentSlice[0] == myRAMSlice)
    {
      // Normally, a write to the read port won't do anything
      // However, the patch command is special in that ignores such
      // cart restrictions
      myRAM[address & 0x03FF] = value;
    }
    else
      myImage[(myCurrentSlice[0] << 11) + (address & (BANK_SIZE-1))] = value;
  }
  else if(address < 0x0900)
  {
    // Normally, a write to the read port won't do anything
    // However, the patch command is special in that ignores such
    // cart restrictions
    myRAM[0x0400 + (myCurrentRAM << 8) + (address & 0x00FF)] = value;
  }
  else
    myImage[(myCurrentSlice[address >> 11] << 11) + (address & (BANK_SIZE-1))] = value;

  return myBankChanged = true;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
const uInt8* CartridgeMNetwork::getImage(size_t& size) const
{
  size = bankCount() * BANK_SIZE;
  return myImage.get();
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
bool CartridgeMNetwork::save(Serializer& out) const
{
  try
  {
    out.putShortArray(myCurrentSlice.data(), myCurrentSlice.size());
    out.putShort(myCurrentRAM);
    out.putByteArray(myRAM.data(), myRAM.size());
  }
  catch(...)
  {
    cerr << "ERROR: " << name() << "::save" << endl;
    return false;
  }

  return true;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
bool CartridgeMNetwork::load(Serializer& in)
{
  try
  {
    in.getShortArray(myCurrentSlice.data(), myCurrentSlice.size());
    myCurrentRAM = in.getShort();
    in.getByteArray(myRAM.data(), myRAM.size());
  }
  catch(...)
  {
    cerr << "ERROR: " << name() << "::load" << endl;
    return false;
  }

  // Set up the previously used banks for the RAM and segment
  bankRAM(myCurrentRAM);
  bank(myCurrentSlice[0]);

  return true;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
uInt16 CartridgeMNetwork::bankCount() const
{
  return uInt16(mySize >> 11);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
uInt16 CartridgeMNetwork::romSize() const
{
  return bankCount() * BANK_SIZE;
}
