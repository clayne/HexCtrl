/****************************************************************************************
* Copyright � 2018-2020 Jovibor https://github.com/jovibor/                             *
* This is a Hex Control for MFC/Win32 applications.                                     *
* Official git repository: https://github.com/jovibor/HexCtrl/                          *
* This software is available under the "MIT License modified with The Commons Clause".  *
* https://github.com/jovibor/HexCtrl/blob/master/LICENSE                                *
* For more information visit the project's official repository.                         *
****************************************************************************************/
#pragma once
#include <afxcontrolbars.h>  //Standard MFC's controls header.
#include "../CHexCtrl.h"
#include "../../res/HexCtrlRes.h"
#include "CHexEdit.h"

namespace HEXCTRL::INTERNAL {
	class CHexDlgOperations final : public CDialogEx
	{
	public:
		explicit CHexDlgOperations(CWnd* pParent = nullptr) : CDialogEx(IDD_HEXCTRL_OPERATIONS, pParent) {}
		virtual ~CHexDlgOperations() {}
		BOOL Create(UINT nIDTemplate, CHexCtrl* pHexCtrl);
	protected:
		virtual void DoDataExchange(CDataExchange* pDX);
		virtual BOOL OnInitDialog();
		virtual BOOL OnCommand(WPARAM wParam, LPARAM lParam);
		DECLARE_MESSAGE_MAP()
	private:
		CHexCtrl* GetHexCtrl()const;
	private:
		CHexCtrl* m_pHexCtrl { };
		CHexEdit m_editOR;
		CHexEdit m_editXOR;
		CHexEdit m_editAND;
		CHexEdit m_editSHL;
		CHexEdit m_editSHR;
		CHexEdit m_editAdd;
		CHexEdit m_editSub;
		CHexEdit m_editMul;
		CHexEdit m_editDiv;
		virtual BOOL OnNotify(WPARAM wParam, LPARAM lParam, LRESULT* pResult);
		virtual void OnOK();
	};
}